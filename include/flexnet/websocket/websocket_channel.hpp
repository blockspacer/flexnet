#pragma once

#include "flexnet/websocket/limited_tcp_stream.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <base/base_export.h>
#include <base/callback.h>
#include <base/location.h>
#include <base/task_runner.h>
#include <base/sequenced_task_runner.h>
#include <base/synchronization/atomic_flag.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flexnet {
namespace ws {

/**
 * A class which represents a single connection
 * When this class is destroyed, the connection is closed.
 * \note does not have reconnect method - must create new to reconnect
 * \see based on
 * https://www.boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
 **/
class WebsocketChannel
  : public std::enable_shared_from_this<WebsocketChannel>
{
public:
  // NOTE: Queue must be created with a fixed maximum size
  // We use Queue per connection
  static const size_t kMaxSendQueueSize = 120;

  // Set the maximum incoming and outgoing message size
  // i.e. the largest permissible message size.
  // Used by |stream.read_message_max(...)|
  static const size_t kMaxMessageSizeBytes = 100000;

 public:
  using MessageBufferType
    = boost::beast::flat_static_buffer<kMaxMessageSizeBytes>;

  template<class Body, class Allocator>
  using UpgradeRequestType
    = boost::beast::http::request<
        Body
        , boost::beast::http::basic_fields<Allocator>
      >;

public:
  WebsocketChannel() = delete;

  WebsocketChannel(
    // Takes ownership of the socket
    ::boost::beast::limited_tcp_stream&& stream
    , ::boost::asio::ssl::context& ctx);

  ~WebsocketChannel();

  /// \note you must set stream options
  /// before performing the handshake.
  void configureStream();

  // Start the asynchronous operation
  template<class Body, class Allocator>
  void start_accept(
    // Clients sends the HTTP request
    // asking for a WebSocket connection
    UpgradeRequestType<Body, Allocator>& req)
  {
    // Accept the websocket handshake
    ws_.async_accept(
      req,
      ::boost::beast::bind_front_handler(
        &WebsocketChannel::on_accept,
        shared_from_this()));
  }

  void on_session_fail(boost::beast::error_code ec, char const* what);

  void on_accept(boost::beast::error_code ec);

  void on_close(::boost::beast::error_code ec);

  void do_read();

  /**
  * @brief starts async writing to client
  */
  void send(const std::shared_ptr<const std::string> message, bool is_binary = true) override;

  /**
  * @brief starts async writing to client
  *
  * @note string will be copied (!!!)
  */
  void send(const std::string& message, bool is_binary = true) override;

  /// \note in multithreaded environment prefer alternative that uses strand
  void send_without_strand(const std::shared_ptr<const std::string> message, bool is_binary = true);

  /// \note in multithreaded environment prefer alternative that uses strand
  void send_copy_without_strand(const std::string& message, bool is_binary = true);

  /// \note session may be flagged as expired on `beast::error::timeout`
  bool isExpired() /*const*/ override;

  void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);

  void on_write(boost::beast::error_code ec, std::size_t bytes_transferred);

  void on_ping(boost::beast::error_code ec);

  //bool isOpenUnsafe();

  bool isOpen()
    override;

  bool fullyCreated() const { return isFullyCreated_; }

  void setFullyCreated(bool isFullyCreated) { isFullyCreated_ = isFullyCreated; }

  void close() override;

  //void closeUnsafe();

  /**
   * @brief returns true if last received message was binary
   */
  bool got_binary() const { return ws_.got_binary(); }

  boost::beast::websocket::stream<::boost::beast::limited_tcp_stream>&
    ref_stream();

private:
  /// \note Only one asynchronous operation can be active at once, see https://github.com/boostorg/beast/issues/1092#issuecomment-379119463
  /// It just means that you cannot call two initiating functions (names that start with async_) on the same object from different threads simultaneously.
  // So after each async_write we need to async_write data scheduled in queue.
  void write_queued();

public:
  /// \note callback will be called when new message is received
  net::DataReadCallback<
    ws::WebsocketChannel,
  > on_data;

  std::function<
    void(ws::WebsocketChannel*)
  > on_destruction;

  /// \note must be called both in case of error
  /// and as result of `close()` called by user
  net::FailCallback<
    std::shared_ptr<ws::WebsocketChannel>
  > fail_handler;

  std::function<
    bool(std::shared_ptr<ws::WebsocketChannel>, ::boost::beast::error_code*)
  > on_accept_;

  std::function<
    bool(std::shared_ptr<ws::WebsocketChannel>)
  > on_before_close_;

  //base::AtomicFlag is_after_close_handled;

  std::function<
    void(std::shared_ptr<ws::WebsocketChannel>, ::boost::beast::error_code*)
  > on_after_close_;

  std::function<
    bool(std::shared_ptr<ws::WebsocketChannel>, ::boost::beast::error_code*, std::size_t)
  > on_before_read_;

  std::function<
    bool(std::shared_ptr<ws::WebsocketChannel>, ::boost::beast::error_code*, std::size_t)
  > on_before_write_;

  std::function<
    bool(std::shared_ptr<ws::WebsocketChannel>, const std::shared_ptr<const std::string>)
  > on_before_send_;

private:

  bool isFullyCreated_{false};

  bool isExpired_{false};

  /**
   * The websocket::stream class template
   * provides asynchronous and blocking message-oriented
   * functionality necessary for clients and servers
   * to utilize the WebSocket protocol.
   * @note all asynchronous operations are performed
   * within the same implicit or explicit strand.
   **/
  boost::beast::websocket::stream<
    // stream with custom rate limiter
    ::boost::beast::limited_tcp_stream
  > ws_;

  /// \todo support both SslWebsocketSession and PlainWebsocketSession https://github.com/iotaledger/hub/blob/master/common/http_server_base.cc#L203

  /**
   * I/O objects such as sockets and streams are not thread-safe. For efficiency, networking adopts
   * a model of using threads without explicit locking by requiring all access to I/O objects to be
   * performed within a strand.
   */
  //boost::asio::strand<boost::asio::io_context::executor_type> strand_;

  // resolver for connection as client
  //boost::asio::ip::tcp::resolver resolver_;

  MessageBufferType buffer_;

  //boost::asio::steady_timer timer_;

  ::boost::asio::ssl::context& ctx_;

  bool isSendBusy_;

  struct QueuedMessage {
    std::shared_ptr<const std::string> data;
    bool is_binary;
  };

  // std::vector<std::shared_ptr<const std::string>> sendQueue_;
  /**
   * If you want to send more than one message at a time, you need to implement
   * your own write queue.
   * @see github.com/boostorg/beast/issues/1207
   *
   * @note ProducerConsumerQueue is a one producer and one consumer queue
   * without locks.
   **/
  /// \todo we use strand, so can use queue without synchronization overhead
  basis::LockFreeProducerConsumerQueue<QueuedMessage> sendQueue_{kMaxSendQueueSize};

  SEQUENCE_CHECKER(sequence_checker_);
};

} // namespace ws
} // namespace flexnet
