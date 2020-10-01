#include "flexnet/websocket/ws_channel.hpp" // IWYU pragma: associated
#include "flexnet/util/mime_type.hpp"
#include "flexnet/ECS/tags.hpp"
#include "flexnet/ECS/components/tcp_connection.hpp"
#include "flexnet/util/close_socket_unsafe.hpp"

#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/threading/thread.h>
#include <base/guid.h>
#include <base/task/thread_pool/thread_pool.h>

#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/promise/post_promise.h>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/assert.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/bind_executor.hpp>

#include <algorithm>
#include <ratio>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

// from <boost/beast.hpp>
namespace beast = boost::beast;
// from <boost/beast/websocket.hpp>
namespace websocket = beast::websocket;
// from <boost/asio/ip/tcp.hpp>
using IpTcp = boost::asio::ip::tcp;
// from <boost/system/error_code.hpp>
using SystemErrorCode = boost::system::error_code;

namespace flexnet {
namespace ws {

WsChannel::WsChannel(
  StreamType&& stream
  , ECS::AsioRegistry& asioRegistry
  , const ECS::Entity entity_id)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , ws_(base::rvalue_cast(COPY_ON_MOVE(stream)))
  , isSendBusy_(false)
  , perConnectionStrand_(
      /// \note `get_executor` returns copy
      ws_.get_executor())
  , asioRegistry_(REFERENCED(asioRegistry))
  , entity_id_(entity_id)
  , sendBuffer_{kMaxSendQueueSize}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  /// \note we assume that configuring stream
  /// is thread-safe here
  /// \note you must set stream options
  /// before performing the handshake.
  {
    /// \todo add SSL support as in
    /// github.com/vinniefalco/beast/blob/master/example/server-framework/main.cpp

    // do not configure working stream
    DCHECK(!ws_.is_open());

    /**
     * Determines if outgoing message payloads are broken up into
     * multiple pieces.
     **/
    ws_.auto_fragment(true);

    /**
     * Permessage-deflate allows messages to be compressed.
     **/
    {
      ::websocket::permessage_deflate pmd;
      pmd.client_enable = true;
      pmd.server_enable = true;
      // Deflate compression level 0..9
      /// \todo make configurable
      pmd.compLevel = 3;
      // Deflate memory level, 1..9
      /// \todo make configurable
      pmd.memLevel = 4;
      ws_.set_option(pmd);
    }

    /**
     * Set the maximum incoming message size option.
     * Message frame fields indicating a size that
     * would bring the total message
     * size over this limit will cause a protocol failure.
     **/
    DCHECK(kMaxMessageSizeByte > 1);
    ws_.read_message_max(kMaxMessageSizeByte);

    // Set a decorator to change the server of the handshake
    ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res)
      {
        res.set(beast::http::field::server,
          /// \todo make configurable
          std::string(BOOST_BEAST_VERSION_STRING) +
            " websocket-server-async");
      }));

    // Turn off the timeout on the tcp_stream, because
    // the websocket stream has its own timeout system.
    beast::get_lowest_layer(ws_).expires_never();

    ///\see https://github.com/boostorg/beast/commit/f21358186ecad9744ed6c72618d4a4cfc36be5fb#diff-68c7d3776da215dd6d1b335448c77f3bR116
    ws_.set_option(websocket::stream_base::timeout{
      // Time limit on handshake, accept, and close operations
      std::chrono::seconds(30), /// \todo make configurable
      // The time limit after which a connection is considered idle.
      std::chrono::seconds(300), /// \todo make configurable
      /*
        If the idle interval is set, this setting affects the
        behavior of the stream when no data is received for the
        timeout interval as follows:
        @li When `keep_alive_pings` is `true`, an idle ping will be
        sent automatically. If another timeout interval elapses
        with no received data then the connection will be closed.
        An outstanding read operation must be pending, which will
        complete immediately the error @ref beast::error::timeout.
        @li When `keep_alive_pings` is `false`, the connection will be closed.
        An outstanding read operation must be pending, which will
        complete immediately the error @ref beast::error::timeout.
      */
      /// \note Prefer to set keep_alive_pings as true on server
      /// and false on client.
      true
    });
  }
}

WsChannel::~WsChannel()
{
  DCHECK_RUN_ON_ANY_THREAD_SCOPE(FUNC_GUARD(WsChannelDestructor));

  LOG_CALL(DVLOG(99));

  /// \note we assume that reading unused `ws_` is thread-safe here
  /// \note don't call `close()` from destructor, handle `close()` manually
  DCHECK(!ws_.is_open());
}

void WsChannel::onFail(
  ErrorCode ec
  , char const* what) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  // log errors with different log levels
  // (log level based on error code)
  {
    // ssl::error::stream_truncated, also known as an SSL "short read",
    // indicates the peer closed the connection without performing the
    // required closing handshake (for example, Google does this to
    // improve performance). Generally this can be a security issue,
    // but if your communication protocol is self-terminated (as
    // it is with both HTTP and WebSocket) then you may simply
    // ignore the lack of close_notify.
    //
    // https://github.com/boostorg/beast/issues/38
    //
    // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
    //
    // When a short read would cut off the end of an HTTP message,
    // Beast returns the error beast::http::error::partial_message.
    // Therefore, if we see a short read here, it has occurred
    // after the message has been completed, so it is safe to ignore it.
    if(ec == ::boost::asio::ssl::error::stream_truncated)
    {
      DVLOG(99)
        << "WsChannel::onFail (stream_truncated): "
        << what
        << " : "
        << ec.message();
    }
    else if (ec == ::boost::asio::error::operation_aborted
        || ec == ::boost::beast::websocket::error::closed)
    {
      DVLOG(99)
        << "WsChannel::onFail (operation_aborted or closed): "
        << what
        << " : "
        << ec.message();
    }
    else if (ec == ::boost::beast::error::timeout)
    {
      DVLOG(99)
        << "WsChannel::onFail (timeout): "
        << what
        << " : "
        << ec.message();
    } else {
      LOG(WARNING)
        << "WsChannel::onFail: "
        << what
        << " : "
        << ec.message();
    }
  }

  doEof();
}

void WsChannel::onAccept(ErrorCode ec) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  if (ec)
  {
    onFail(ec, "accept");
    return;
  }

  // Read a message
  doRead();
}

void WsChannel::doRead() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  // Clear the buffer
  readBuffer_.consume(readBuffer_.size());

  // Read a message into our buffer
  ws_.async_read(
      readBuffer_,
      boost::asio::bind_executor(
        *perConnectionStrand_
        , ::std::bind(
            &WsChannel::onRead,
            UNOWNED_LIFETIME(
              this)
            , std::placeholders::_1
            , std::placeholders::_2
          )
      )
      /*beast::bind_front_handler(
          &WsChannel::onRead,
          this)*/);
}

void WsChannel::onClose(ErrorCode ec) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  if(ec)
  {
    /// \todo prevent infinite recursion
    /// onFail -> doEof -> async_close -> onClose -> onFail
    onFail(ec, "close");
    return;
  }

  // If we get here then the connection is closed gracefully
}

void WsChannel::doEof() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(asioRegistry_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(entity_id_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  auto& socket
    = beast::get_lowest_layer(ws_).socket();

  // Close the WebSocket connection
  if(ws_.is_open())
  {
    // Set the timeout.
    beast::get_lowest_layer(ws_)
      .expires_after(std::chrono::seconds(kCloseTimeoutSec));

    DVLOG(99)
      << "WsChannel::do_eof for remote_endpoint: "
      /// \note Transport endpoint must be connected i.e. `is_open()`
      << socket.remote_endpoint();

    ws_.async_close(websocket::close_code::normal,
      boost::asio::bind_executor(
        *perConnectionStrand_
        , ::std::bind(
            &WsChannel::onClose,
            UNOWNED_LIFETIME(
              this)
            , std::placeholders::_1
          )
      ));
  }

  ECS::Registry* registryPtr = nullptr;
  {
    DCHECK_RUN_ON_ANY_THREAD_SCOPE(asioRegistry_->FUNC_GUARD(registry));
    registryPtr = &(asioRegistry_->registry());
  }

  // mark SSL detection completed
  ::boost::asio::post(
    asioRegistry_->asioStrand()
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        base::rvalue_cast(task).Run();
      }
      , base::BindOnce(
          [
          ](
            /// \note copy because `this`
            /// i.e. `entity_id_` may be freed
            ECS::EntityId entity_id
            /// \note copy pointer because `this`
            /// i.e. `asioRegistry_` may be freed
            /// \note must control lifetime
            , ECS::Registry* registryPtr
          )
          {
            LOG_CALL(DVLOG(99));

            if(!registryPtr->valid(entity_id)) {
              return;
            }

            if(!registryPtr->has<ECS::UnusedTag>(entity_id)) {
              registryPtr->emplace<ECS::UnusedTag>(entity_id);
            }
          }
          , entity_id_
          , registryPtr
        )
    )
  );
}

void WsChannel::onRead(
  ErrorCode ec
  , std::size_t bytes_transferred) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  boost::ignore_unused(bytes_transferred);

  // Handle the error, if any
  if(ec) {
    onFail(ec, "read");
    return;
  }

  if (!readBuffer_.size())
  {
    // may be empty if connection reset by peer
    DVLOG(99)
      << "WebsocketChannel::on_read:"
         " empty messageBuffer";
    return;
  }

  if (readBuffer_.size() > kMaxMessageSizeByte)
  {
    DVLOG(99)
      << "WebsocketChannel::on_read:"
         " Too big messageBuffer of size "
      << readBuffer_.size();
    return;
  }

  const std::string data
    = beast::buffers_to_string(readBuffer_.data());
  DVLOG(99)
    << "WsSession on_read: "
    << data.substr(0, 1024);

  /// \todo on_data(shared_from_this(), getId(), data);

  // Clear the buffer
  readBuffer_.consume(readBuffer_.size());

  // Do another read
  doRead();
}

bool WsChannel::isOpen() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  return ws_.is_open();
}

void WsChannel::sendAsync(
  SharedMessageData message
  , bool is_binary) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_ANY_THREAD_SCOPE(FUNC_GUARD(send));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(message);

  if (message->empty())
  {
    LOG_CALL(DVLOG(99))
      << "unable to send empty message";
    return;
  }

  if (message->size() > kMaxMessageSizeByte)
  {
    LOG_CALL(DVLOG(99))
      << "unable to send too big message of size "
      << message->size();
    return;
  }

  ::boost::asio::post(
    *perConnectionStrand_
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
        ](
          base::OnceClosure&& task
        ){
          DCHECK(task);
          base::rvalue_cast(task).Run();
        }
        , base::BindOnce(
          &WsChannel::send
          , base::Unretained(this)
          , message
          , is_binary
        )
      )
    /*, boost::asio::bind_executor(
      *perConnectionStrand_
      , ::std::bind(
          &WsChannel::startAccept<Body, Allocator>,
          UNOWNED_LIFETIME(
            this)
          , base::rvalue_cast(req)
        )
    )*/
  );
}

void WsChannel::send(
  SharedMessageData message
  , bool is_binary) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  DCHECK(message);

  DCHECK(!message->empty());

  DCHECK(message->size() <= kMaxMessageSizeByte);

  if (sendBuffer_.isFull())
  {
    LOG_CALL(LOG(WARNING))
      << "Server send buffer is full."
      << "That may indicate DDOS or configuration issues.";

    // FALLTHROUGH: it is ok to write at full circular buffer
  }

  // Expect that `writeBack` will remove OLDEST data
  // in circular buffer to free space for new message.
  sendBuffer_.writeBack(
    QueuedMessage{message, is_binary});

  if(isSendBusy_)
  {
    // expect that `writeQueued` will be called again
    // after `async_write` finished.
    return;
  }

  // sanity check
  DCHECK(!sendBuffer_.isEmpty());

  writeQueued();
}

void WsChannel::writeQueued() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  DCHECK(isSendBusy_);

  DCHECK(!sendBuffer_.isEmpty());

  DCHECK(sendBuffer_.backPtr());

  QueuedMessage dp{};

  // expect that `backPtr` will return NEWEST data
  /// \note We do not remove OLDEST messages manually (via `popFront`)
  /// because we use circular buffer.
  dp
    /// \note performs copy,
    /// take care of performance
    = (*sendBuffer_.backPtr());

  /// \note we don't allow sending empty messages
  DCHECK(dp.data.get() && !dp.data->empty());

  // This controls whether or not outgoing messages
  // are set to binary or text.
  /// \note Because we call `writeQueued` sequentially,
  /// `ws_.binary` expected to be not changed until
  /// `writeQueued` (i.e. `async_write`) completion.
  ws_.binary(dp.is_binary);

  ws_.async_write(
    ::boost::asio::buffer(
      /// \todo performs copy
      *(dp.data)),
    /// \todo use base::BindFrontWrapper
    ::boost::asio::bind_executor(
      *perConnectionStrand_
      , ::std::bind(
          &WsChannel::onWrite,
          UNOWNED_LIFETIME(
            this)
          , std::placeholders::_1
          , std::placeholders::_2
        )
    )
  );
}

void WsChannel::onWrite(
  ErrorCode ec
  , std::size_t bytes_transferred) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

  DCHECK(perConnectionStrand_->running_in_this_thread());

  /// \note Because we call `writeQueued` sequentially,
  /// `isSendBusy_` expected to be not changed until
  /// `writeQueued` (i.e. `async_write`) completion.
  DCHECK(isSendBusy_);

  if (ec) {
    onFail(ec, "write");
    return;
  }

  DVLOG(99)
    << "bytes sent: "
    << bytes_transferred;

  /// \todo on_before_write_(shared_from_this(), &ec, bytes_transferred))

  if (!sendBuffer_.isEmpty())
  {
    // Schedule `async_write`
    // for next unprocessed message in queue.
    writeQueued();
  }
  else
  {
    isSendBusy_ = false;
  }
}

} // namespace ws
} // namespace flexnet
