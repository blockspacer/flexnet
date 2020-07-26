#pragma once

/**
 * \note session does not have reconnect method - must create new session
 * \see https://www.boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
 **/

#include <net/http/SessionGUID.hpp>
#include <net/SessionBase.hpp>
#include <net/FailCallback.hpp>
#include <net/limited_tcp_stream.hpp>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/ssl.hpp>
#include <cstddef>
#include <cstdint>
#include <net/http/server/DetectSession.hpp>
#include <string>
#include <vector>
#include <optional>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace gloer {
namespace algo {
class DispatchQueue;
} // namespace algo
} // namespace gloer

namespace gloer {
namespace net {

} // namespace net
} // namespace gloer

namespace gloer {
namespace net {
namespace http {

/**
 * A class which represents a single connection
 * When this class is destroyed, the connection is closed.
 **/
class ServerSession
  : public SessionBase<http::SessionGUID>, public std::enable_shared_from_this<ServerSession> {
private:
  // NOTE: ProducerConsumerQueue must be created with a fixed maximum size
  // We use Queue per connection
  static const size_t MAX_SENDQUEUE_SIZE = 120;
  static const size_t MAX_BUFFER_BYTES_SIZE = 100000;

public:
    using request_body_t = boost::beast::http::string_body;
    using buffer_t = boost::beast::flat_static_buffer<MAX_BUFFER_BYTES_SIZE>;

public:
  ServerSession() = delete;

  // Take ownership of the socket
  explicit ServerSession(::boost::beast::limited_tcp_stream&& stream,
    gloer::net::http::DetectSession::buffer_t&& buffer,
    ::boost::asio::ssl::context& ctx,
    const http::SessionGUID& id);

  ~ServerSession();

  /// \note `stream_` can be moved to websocket session from http session,
  /// so we can't use it here anymore
  MUST_USE_RETURN_VALUE
  bool is_stream_valid() const {
    return http_stream_valid_.load();
  }

  void on_session_fail(boost::beast::error_code ec, char const* what);

  void do_read();

  void do_eof();

  void close();

  void on_shutdown(::boost::beast::error_code ec);

  /**
  * @brief starts async writing to client
  *
  * @note string will be copied (!!!)
  */
  void send(const std::string& message, bool is_binary = true) /*override*/;

  void send(const std::shared_ptr<const std::string> shared_msg, bool is_binary = true) /*override*/;

  MUST_USE_RETURN_VALUE
  bool isExpired() /*const*/ /*override*/;

  MUST_USE_RETURN_VALUE
  bool isOpen() /*const*/ /*override*/;

  void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);

  void on_write(boost::beast::error_code ec, std::size_t bytes_transferred, bool close);

  std::function<
    void(http::ServerSession*)
  > on_destruction;

  net::FailCallback<
    std::shared_ptr<http::ServerSession>
  > fail_handler;

  std::function<
    const std::optional<::boost::beast::http::response<request_body_t>>
      (std::shared_ptr<http::ServerSession>, ::boost::beast::error_code*, std::size_t)
  > on_websocket_upgrade_;

  /// \note stream with custom rate limiter
  ::boost::beast::limited_tcp_stream stream_;

  /// \todo support both SslHttpSession and PlainHttpSession https://github.com/iotaledger/hub/blob/master/common/http_server_base.cc#L441

  /// \note `stream_` can be moved to websocket session from http session,
  /// so we can't use it here anymore
  std::atomic<bool> http_stream_valid_ = true;

  // The parser is stored in an optional container so we can
  // construct it from scratch it at the beginning of each new message.
  boost::optional<::boost::beast::http::request_parser<request_body_t>> parser_;

private:
  static size_t MAX_IN_MSG_SIZE_BYTE;
  static size_t MAX_OUT_MSG_SIZE_BYTE;

  buffer_t buffer_;

  // The timer putting a time limit on requests.
  //::boost::asio::steady_timer request_deadline_;

  ::boost::asio::ssl::context& ctx_;
};

} // namespace http
} // namespace net
} // namespace gloer
