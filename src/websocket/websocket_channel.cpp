#include "flexnet/websocket/websocket_channel.hpp" // IWYU pragma: associated

#include "flexnet/util/macros.hpp"

#include "algo/DispatchQueue.hpp"
#include <basis/log/Logger.hpp>
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/assert.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/system/error_code.hpp>
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

#include <base/bind.h>
#include <base/logging.h>

#include <basis/task_run/task_run_util.hpp>

namespace beast = boost::beast;               // from <boost/beast.hpp>
//namespace http = beast::http;                 // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;       // from <boost/beast/websocket.hpp>
//namespace net = boost::asio;                  // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;             // from <boost/asio/ip/tcp.hpp>
using error_code = boost::system::error_code; // from <boost/system/error_code.hpp>

namespace flexnet {
namespace ws {

WebsocketChannel::WebsocketChannel(
  ::boost::beast::limited_tcp_stream&& stream
  , ::boost::asio::ssl::context& ctx)
    : ctx_(ctx)
      // NOTE: Following the std::move,
      // the moved-from object is in the same state
      // as if constructed using the
      // basic_stream_socket(io_service&) constructor.
      // see boost.org/doc/libs/1_54_0/doc/html/boost_asio/reference/basic_stream_socket/basic_stream_socket/overload5.html
      // i.e. it does not actually destroy |stream| by |move|
      , ws_(std::move(stream))
      , isSendBusy_(false)
{
  DETACH_FROM_SEQUENCE(sequence_checker_);

  configureStream();
}

WebsocketChannel::~WebsocketChannel()
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  //DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  /// \note destructor may be called on random thread
  /// \todo use RefCountedDeleteOnSequence https://github.com/blockspacer/chromium_base_conan/blob/8e45a5dc6abfc06505fd660c08ad43c592daf5aa/base/memory/ref_counted_delete_on_sequence.h

  DCHECK(on_destruction);
  on_destruction(this); /// \note no shared_ptr in destructor

  //const ws::SessionGUID wsConnId = getId(); // remember id before session deletion

  /// \note don't call `close()` from destructor, handle `close()` manually
  DCHECK(!ws_.is_open());
}

/// \todo add SSL support as in
/// github.com/vinniefalco/beast/blob/master/example/server-framework/main.cpp
void WebsocketChannel::configureStream()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

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
    pmd.compLevel = 3;
    // Deflate memory level, 1..9
    pmd.memLevel = 4;
  }
  ws_.set_option(pmd);

  /**
   * Set the maximum incoming message size option.
   * Message frame fields indicating a size that
   * would bring the total message
   * size over this limit will cause a protocol failure.
   **/
  DCHECK(kMaxMessageSizeBytes > 1);
  ws_.read_message_max(kMaxMessageSizeBytes);

  // Set a decorator to change the server of the handshake
  ws_.set_option(websocket::stream_base::decorator(
    [](websocket::response_type& res)
    {
      res.set(beast::http::field::server,
        std::string(BOOST_BEAST_VERSION_STRING) +
          " websocket-server-async");
    }));

  // Turn off the timeout on the tcp_stream, because
  // the websocket stream has its own timeout system.
  beast::get_lowest_layer(ws_).expires_never();

  ///\see https://github.com/boostorg/beast/commit/f21358186ecad9744ed6c72618d4a4cfc36be5fb#diff-68c7d3776da215dd6d1b335448c77f3bR116
  ws_.set_option(websocket::stream_base::timeout{
    // Time limit on handshake, accept, and close operations
    std::chrono::seconds(30),
    // The time limit after which a connection is considered idle.
    std::chrono::seconds(300),
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

void WebsocketChannel::on_session_fail(
  beast::error_code ec, char const* what)
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  /*if(!isOpen()
     && ec != ::websocket::error::closed)
  {
    /// \note we didn't call close,
    /// but want to handle close event anyway
    DCHECK(!is_after_close_handled.IsSet());
    is_after_close_handled.Set();
    DCHECK(on_after_close_);
    on_after_close_(shared_from_this(), &ec);
  }*/

  DCHECK(fail_handler);
  /// \note user can provide custom timeout handler e.t.c.
  if (!fail_handler(shared_from_this(), &ec, what)) {
    /// \note skipped |on_after_close_()|
    return;
  }

  // Don't report these
  if (ec == ::websocket::error::closed) {
    DCHECK(!isOpen());
    return;
  }

  if(isOpen()) {
    close();
  }
  DCHECK(!isOpen());

  if (ec == ::boost::asio::error::operation_aborted) {
    return;
  }

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
  if(ec == ::boost::asio::ssl::error::stream_truncated) {
      return;
  }

  if (ec == beast::error::timeout) {
      isExpired_ = true;
      return;
  }
}

void WebsocketChannel::on_accept(beast::error_code ec)
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  if (ec)
    return on_session_fail(ec, "accept");

  if(!on_accept_(shared_from_this(), &ec)) {
    return;
  }

  setFullyCreated(true); // TODO

  // Read a message
  do_read();
}

void WebsocketChannel::close()
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  LOG(INFO)
    << "called WebsocketChannel::close"
    << " for session "
    << static_cast<std::string>(getId());

  //async_task_on_executor(
  //  [this]() {
      DCHECK(isOpen());

      /*boost::system::error_code errorCode;
      ws_.close(boost::beast::websocket::close_reason(boost::beast::websocket::close_code::normal),
                errorCode);
      if (errorCode) {
        LOG(WARNING) << "WsSession: Close error: " << errorCode.message();
      }*/

      DCHECK(on_before_close_);
      if(!on_before_close_(shared_from_this())) {
        return;
      }

      // Close the WebSocket connection
      ws_.async_close(websocket::close_code::normal,
          beast::bind_front_handler(
              &WebsocketChannel::on_close,
              shared_from_this()));
  //  }
  //);
}

//void WebsocketChannel::closeUnsafe()
//{
//  DCHECK(on_before_close_);
//  if(!on_before_close_(shared_from_this())) {
//    return;
//  }
//
//  // Close the WebSocket connection
//  ws_.async_close(websocket::close_code::normal,
//      beast::bind_front_handler(
//          &WebsocketChannel::on_close,
//          shared_from_this()));
//}

boost::beast::websocket::stream<beast::limited_tcp_stream>&
  WebsocketChannel::ref_stream()
{
  return ws_;
}

void WebsocketChannel::on_close(beast::error_code ec)
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  //DCHECK(!is_after_close_handled.IsSet());
  //is_after_close_handled.Set();
  DCHECK(on_after_close_);
  on_after_close_(shared_from_this(), &ec);

  if(ec)
    return on_session_fail(ec, "close");

  // If we get here then the connection is closed gracefully

  // The make_printable() function helps print a ConstBufferSequence
  // LOG(INFO) << beast::make_printable(buffer_.data());
}

void WebsocketChannel::do_read()
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  // LOG(INFO) << "WS session do_read";

  // Set the timer
  // timer_.expires_after(std::chrono::seconds(WS_PING_FREQUENCY_SEC));

  /*if (!isOpen()) {
    LOG(WARNING) << "WebsocketChannel !sess->isOpen";
    //on_session_fail(ec, "timeout");
    ws::SessionGUID copyId = getId();
    nm_->sessionManager().unregisterSession(copyId);
    return;
  }*/

  // Read a message into our buffer
  /*ws_.async_read(
      buffer_,
      ::boost::asio::bind_executor(ws_.get_executor(), std::bind(&WebsocketChannel::on_read, shared_from_this(),
                                              std::placeholders::_1, std::placeholders::_2)));
  */

  // Clear the buffer
  buffer_.consume(buffer_.size());

  // Read a message into our buffer
  ws_.async_read(
      buffer_,
      beast::bind_front_handler(
          &WebsocketChannel::on_read,
          shared_from_this()));
}

void WebsocketChannel::on_read(
  beast::error_code ec, std::size_t bytes_transferred)
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  boost::ignore_unused(bytes_transferred);

  if (ec)
    return on_session_fail(ec, "read");

  // LOG(INFO) << "WS session on_read";

  DCHECK(on_before_read_);
  if(!on_before_read_(shared_from_this(), &ec, bytes_transferred)) {
    return;
  }

  // Note that there is activity
  // onRemoteActivity();

  // LOG(INFO) << "buffer: " <<
  // beast::buffers_to_string(received_buffer_.data());
  // send(beast::buffers_to_string(received_buffer_.data())); // ??????

  if (!buffer_.size()) {
    // may be empty if connection reset by peer
    LOG(WARNING) << "WebsocketChannel::on_read: empty messageBuffer";
    return;
  }

  DCHECK(kMaxMessageSizeBytes > 1);
  if (buffer_.size() > kMaxMessageSizeBytes) {
    LOG(WARNING) << "WebsocketChannel::on_read: Too big messageBuffer of size " << buffer_.size();
    return;
  }

  /*if (!ws_.got_binary()) {
    LOG(WARNING) << "ws/server/WebsocketChannel: !ws_.got_binary() ";
  }*/

  // add incoming message callback into queue
  // TODO: use protobuf
  /*auto sharedBuffer =
      std::make_shared<std::string>(beast::buffers_to_string(buffer_.data()));*/

  // handleIncomingJSON(sharedBuffer);

  // handleIncomingJSON(data);

  DCHECK(on_data);
  const std::string data = beast::buffers_to_string(buffer_.data());
  LOG(WARNING) << "WsSession on_read: " << data.substr(0, 1024);
  on_data(shared_from_this(), getId(), data);

  // Clear the buffer
  buffer_.consume(buffer_.size());

  // Do another read
  do_read();
}

//bool WebsocketChannel::isOpenUnsafe() /*const*/
//{
//  return ws_.is_open();
//}

bool WebsocketChannel::isOpen() /*const*/
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  return ws_.is_open();
}

void WebsocketChannel::on_write(
  beast::error_code ec, std::size_t bytes_transferred)
{
  DCHECK(isSendBusy_);

  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  if (ec) {
    LOG(WARNING) << "WsSession on_write: ec";
    return on_session_fail(ec, "write");
  }

  DCHECK(on_before_write_);
  if(!on_before_write_(shared_from_this(), &ec, bytes_transferred)) {
    return;
  }

  if (!sendQueue_.isEmpty()) {
    // Remove the already written string from the queue
    sendQueue_.popFront();
  }

  if (!sendQueue_.isEmpty()) {
    write_queued();
  } else {
    LOG(INFO) << "write send_queue_.empty()";
    isSendBusy_ = false;
  }
}

void WebsocketChannel::write_queued()
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  DCHECK(isSendBusy_);
  DCHECK(!sendQueue_.isEmpty());

  /// \note !isEmpty => has valid frontPtr
  DCHECK(sendQueue_.frontPtr());

  /// \note we don't allow sending empty messages
  DCHECK(sendQueue_.frontPtr()->data.get());
  DCHECK(!sendQueue_.frontPtr()->data->empty());

  QueuedMessage dp{};
  sendQueue_.read(dp);

  /// \note we don't allow sending empty messages
  DCHECK(dp.data.get());
  DCHECK(!dp.data->empty());

  // This controls whether or not outgoing messages are set to binary or text.
  ws_.binary(dp.is_binary);

  ws_.async_write(
      ::boost::asio::buffer(*(dp.data)),
      beast::bind_front_handler(
                      &WebsocketChannel::on_write,
                      shared_from_this()));
}

void WebsocketChannel::send(
  const std::shared_ptr<const std::string> shared_msg
  , bool is_binary)
{
  // Post our work to the strand, to prevent data race
  boost::asio::post(
    ws_.get_executor(), /// \todo is it thread-safe to get ws_ here?
    beast::bind_front_handler(
      &WebsocketChannel::send_without_strand,
      shared_from_this(),
      shared_msg,
      is_binary)
  );
}

void WebsocketChannel::send(
  const std::string& message, bool is_binary)
{
  /// \note string copied
  std::shared_ptr<const std::string> shared_msg =
      std::make_shared<const std::string>(message);

  // repost task on asio executor
  DCHECK(!basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  // Post our work to the strand, to prevent data race
  boost::asio::post(
    ws_.get_executor(), /// \todo is it thread-safe to get ws_ here?
    beast::bind_front_handler(
      &WebsocketChannel::send_copy_without_strand,
      shared_from_this(),
      message,
      is_binary)
  );
}

void WebsocketChannel::send_copy_without_strand(
  const std::string& message, bool is_binary)
{
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  /// \note string copied
  std::shared_ptr<const std::string> shared_msg =
      std::make_shared<const std::string>(message);

  send_without_strand(shared_msg, is_binary);
}

void WebsocketChannel::send_without_strand(
  const std::shared_ptr<const std::string> shared_msg, bool is_binary)
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  DCHECK(on_before_send_);
  if(!on_before_send_(shared_from_this(), shared_msg)) {
    return;
  }

  if (shared_msg->empty()) {
    LOG(WARNING) << "WebsocketChannel::send: empty messageBuffer";
    return;
  }

  DCHECK(kMaxMessageSizeBytes > 1);
  if (shared_msg->size() > kMaxMessageSizeBytes) {
    LOG(WARNING) << "WebsocketChannel::send: Too big messageBuffer of size " << shared_msg->size();
    return;
  }

  if (!sendQueue_.isFull()) {
    sendQueue_.write(QueuedMessage{shared_msg, is_binary});
  } else {
    // Too many messages in queue
    LOG(WARNING) << "server send_queue_ isFull!";
    // TODO: add event: on_send_queue_overflow
    return;
  }

  if (!isSendBusy_ && !sendQueue_.isEmpty()) {
    isSendBusy_ = true;

    write_queued();
  }
}

bool WebsocketChannel::isExpired() /*const*/
{
  //DCHECK(task_runner_);
  //DCHECK(task_runner_->RunsTasksInCurrentSequence());
  //DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(basis::RunsTasksInAnySequenceOf(allowed_task_runners_));

  return isExpired_;
}

} // namespace ws
} // namespace flexnet