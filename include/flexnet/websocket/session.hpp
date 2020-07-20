#pragma once

/**
 * \note session does not have reconnect method - must create new session
 * \see https://www.boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
 **/

#include "net/SessionBase.hpp"
#include <net/limited_tcp_stream.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "net/FailCallback.hpp"
#include "net/ws/SessionGUID.hpp"
#include "net/DataReadCallback.hpp"
#include <basis/concurrency/LockFreeProducerConsumerQueue.hpp>
#include <base/synchronization/atomic_flag.h>

#include <base/base_export.h>
#include <base/callback.h>
#include <base/location.h>
#include <base/task_runner.h>
#include <base/sequenced_task_runner.h>
//#include <base/memory/ref_counted.h>
//#include <base/time/time.h>

namespace gloer {
namespace algo {
class DispatchQueue;
} // namespace algo
} // namespace gloer

namespace flexnet {
namespace ws {

/**
 * A class which represents a single connection
 * When this class is destroyed, the connection is closed.
 **/
class ServerSession
  : public SessionBase<ws::SessionGUID>
  , public std::enable_shared_from_this<ServerSession>
{
public:
  // NOTE: ProducerConsumerQueue must be created with a fixed maximum size
  // We use Queue per connection
  static const size_t MAX_SENDQUEUE_SIZE = 120;
  static const size_t MAX_BUFFER_BYTES_SIZE = 100000;

 public:
  using buffer_t = boost::beast::flat_static_buffer<MAX_BUFFER_BYTES_SIZE>;

public:
  ServerSession() = delete;

  // Take ownership of the socket
  explicit ServerSession(::boost::beast::limited_tcp_stream&& stream,//boost::asio::ip::tcp::socket&& socket,
    //beast::flat_buffer&& buffer,
    ::boost::asio::ssl::context& ctx,
    const ws::SessionGUID& id);

  ~ServerSession();

  // Start the asynchronous operation
  template<class Body, class Allocator>
  void start_accept(
    boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>>& req)
  {
    // Set suggested timeout settings for the websocket
    ws_.set_option(
        ::boost::beast::websocket::stream_base::timeout::suggested(
            ::boost::beast::role_type::server));

    // Set a decorator to change the Server of the handshake
    ws_.set_option(::boost::beast::websocket::stream_base::decorator(
        [](::boost::beast::websocket::response_type& res)
        {
            res.set(boost::beast::http::field::server,
                std::string(BOOST_BEAST_VERSION_STRING) +
                    " websocket-chat-multi");
        }));

    // Accept the websocket handshake
    ws_.async_accept(
        req,
        ::boost::beast::bind_front_handler(
            &ServerSession::on_accept,
            shared_from_this()));
  }

  void on_session_fail(boost::beast::error_code ec, char const* what);

#if 0
  void on_control_callback(boost::beast::::boost::beast::websocket::frame_type kind,
                           boost::string_view payload); // TODO boost::string_view or
                                                        // std::string_view

  // Called to indicate activity from the remote peer
  void onRemoteActivity();

  // Called when the timer expires.
  void on_timer(boost::beast::error_code ec);
#endif // 0

  void on_accept(boost::beast::error_code ec);

  void on_close(::boost::beast::error_code ec);

  void do_read();

  void async_task_on_executor(
    std::function<void()>&& task);

  void setAllowedTaskRunners(
    std::vector<scoped_refptr<base::SequencedTaskRunner>> allowed_task_runners)
    noexcept;

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
    ws::ServerSession,
    ws::SessionGUID
  > on_data;

  std::function<
    void(ws::ServerSession*)
  > on_destruction;

  /// \note must be called both in case of error
  /// and as result of `close()` called by user
  net::FailCallback<
    std::shared_ptr<ws::ServerSession>
  > fail_handler;

  std::function<
    bool(std::shared_ptr<ws::ServerSession>, ::boost::beast::error_code*)
  > on_accept_;

  std::function<
    bool(std::shared_ptr<ws::ServerSession>)
  > on_before_close_;

  //base::AtomicFlag is_after_close_handled;

  std::function<
    void(std::shared_ptr<ws::ServerSession>, ::boost::beast::error_code*)
  > on_after_close_;

  std::function<
    bool(std::shared_ptr<ws::ServerSession>, ::boost::beast::error_code*, std::size_t)
  > on_before_read_;

  std::function<
    bool(std::shared_ptr<ws::ServerSession>, ::boost::beast::error_code*, std::size_t)
  > on_before_write_;

  std::function<
    bool(std::shared_ptr<ws::ServerSession>, const std::shared_ptr<const std::string>)
  > on_before_send_;

private:

  bool isFullyCreated_{false};

  bool isExpired_{false};

  static size_t MAX_IN_MSG_SIZE_BYTE;
  static size_t MAX_OUT_MSG_SIZE_BYTE;

  /**
   * The websocket::stream class template provides asynchronous and blocking message-oriented
   * functionality necessary for clients and servers to utilize the WebSocket protocol.
   * @note all asynchronous operations are performed within the same implicit or explicit strand.
   **/
  /// \note stream with custom rate limiter
  boost::beast::websocket::stream<::boost::beast::limited_tcp_stream> ws_;

  /// \todo support both SslWebsocketSession and PlainWebsocketSession https://github.com/iotaledger/hub/blob/master/common/http_server_base.cc#L203

  /**
   * I/O objects such as sockets and streams are not thread-safe. For efficiency, networking adopts
   * a model of using threads without explicit locking by requiring all access to I/O objects to be
   * performed within a strand.
   */
  //boost::asio::strand<boost::asio::io_context::executor_type> strand_;

  // resolver for connection as client
  //boost::asio::ip::tcp::resolver resolver_;

  buffer_t buffer_;

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
  basis::LockFreeProducerConsumerQueue<QueuedMessage> sendQueue_{MAX_SENDQUEUE_SIZE};

  //scoped_refptr<base::TaskRunner> task_runner_;
  std::vector<scoped_refptr<base::SequencedTaskRunner>> allowed_task_runners_;

  //SEQUENCE_CHECKER(sequence_checker_);

  //uint32_t pingState_ = 0;
};

} // namespace ws
} // namespace flexnet
