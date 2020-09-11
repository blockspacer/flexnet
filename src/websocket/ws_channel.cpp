#include "flexnet/websocket/ws_channel.hpp" // IWYU pragma: associated
#include "flexnet/util/mime_type.hpp"
#include "flexnet/ECS/tags.hpp"
#include "flexnet/ECS/components/close_socket.hpp"
#include "flexnet/ECS/components/tcp_connection.hpp"

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
  : ws_(base::rvalue_cast(COPY_ON_MOVE(stream)))
  , isSendBusy_(false)
  , perConnectionStrand_(
      /// \note `get_executor` returns copy
      ws_.get_executor())
  , buffer_{}
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , asioRegistry_(REFERENCED(asioRegistry))
  , entity_id_(entity_id)
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

NOT_THREAD_SAFE_FUNCTION()
WsChannel::~WsChannel()
{
  LOG_CALL(DVLOG(99));

  /// \note we assume that reading unused `ws_` is thread-safe here
  /// \note don't call `close()` from destructor, handle `close()` manually
  DCHECK(!ws_.is_open());
}

void WsChannel::onFail(
  ErrorCode ec
  , char const* what)
{
  LOG_CALL(DVLOG(99));

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

void WsChannel::onAccept(ErrorCode ec)
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  if (ec)
  {
    onFail(ec, "accept");
    return;
  }

  // Read a message
  doRead();
}

void WsChannel::doRead()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  // Clear the buffer
  buffer_.consume(buffer_.size());

  // Read a message into our buffer
  ws_.async_read(
      buffer_,
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

void WsChannel::onClose(ErrorCode ec)
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  if(ec)
  {
    onFail(ec, "close");
    return;
  }

  // If we get here then the connection is closed gracefully
}

void WsChannel::doEof()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);
  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  auto& socket
    = beast::get_lowest_layer(ws_).socket();

  // Close the WebSocket connection
  if(ws_.is_open())
  {
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
      )
      /*beast::bind_front_handler(
          &WsChannel::onClose,
          this)*/);
  }

  auto closeAndReleaseResources
    = [this, &socket]()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);
    DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
    DCHECK_CUSTOM_THREAD_GUARD(entity_id_);

    DCHECK(asioRegistry_->running_in_this_thread());

    // Schedule shutdown on asio thread
    if(!(*asioRegistry_)->has<ECS::CloseSocket>(entity_id_)) {
      (*asioRegistry_)->emplace<ECS::CloseSocket>(entity_id_
        /// \note lifetime of `acceptResult` must be prolonged
        , UNOWNED_LIFETIME() &socket
        , UNOWNED_LIFETIME() &perConnectionStrand_.data
      );
    }
  };

  // Set the timeout.
  beast::get_lowest_layer(ws_)
    .expires_after(std::chrono::seconds(kCloseTimeoutSec));

  // mark SSL detection completed
  ::boost::asio::post(
    asioRegistry_->strand()
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler(
        base::rvalue_cast(closeAndReleaseResources)
      )
  );
}

void WsChannel::onRead(
  ErrorCode ec
  , std::size_t bytes_transferred)
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  boost::ignore_unused(bytes_transferred);

  // Handle the error, if any
  if(ec) {
    onFail(ec, "read");
    return;
  }

  if (!buffer_.size()) {
    // may be empty if connection reset by peer
    DVLOG(99)
      << "WebsocketChannel::on_read:"
         " empty messageBuffer";
    return;
  }

  DCHECK(kMaxMessageSizeByte > 1);
  if (buffer_.size() > kMaxMessageSizeByte) {
    DVLOG(99)
      << "WebsocketChannel::on_read:"
         " Too big messageBuffer of size "
      << buffer_.size();
    return;
  }

  const std::string data = beast::buffers_to_string(buffer_.data());
  DVLOG(99)
    << "WsSession on_read: "
    << data.substr(0, 1024);

  /// \todo on_data(shared_from_this(), getId(), data);

  // Clear the buffer
  buffer_.consume(buffer_.size());

  // Do another read
  doRead();
}

bool WsChannel::isOpen()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  return ws_.is_open();
}

} // namespace ws
} // namespace flexnet
