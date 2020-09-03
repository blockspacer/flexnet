#include "net/http/server/ServerSession.hpp" // IWYU pragma: associated
#include "net/ws/server/ServerSession.hpp"
#include "algo/DispatchQueue.hpp"

#include <base/logging.h>
#include <base/rvalue_cast.h>

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
#include <net/http/SessionGUID.hpp>
#include <net/http/MimeType.hpp>

#include <base/bind.h>
#include <base/logging.h>

namespace beast = boost::beast;               // from <boost/beast.hpp>
//namespace http = beast::http;                 // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;       // from <boost/beast/websocket.hpp>
//namespace net = boost::asio;                  // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;             // from <boost/asio/ip/tcp.hpp>
using error_code = boost::system::error_code; // from <boost/system/error_code.hpp>

namespace {

// Append an HTTP rel-path to a local filesystem path.
// The returned path is normalized for the platform.
std::string
path_cat(
    beast::string_view base,
    beast::string_view path)
{
    if(base.empty())
        return std::string(path);
    std::string result(base);
#ifdef BOOST_MSVC
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<
    class Body, class Allocator,
    class Send>
void
handle_request(
    beast::string_view doc_root,
    beast::http::request<Body, beast::http::basic_fields<Allocator>>&& req,
    const std::optional<beast::http::response<gloer::net::http::ServerSession::request_body_t>>& custom_response,
    Send&& send)
{
    // Returns a bad request response
    auto const bad_request =
    [&req](beast::string_view why)
    {
        beast::http::response<gloer::net::http::ServerSession::request_body_t> res{beast::http::status::bad_request, req.version()};
        res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(beast::http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = std::string(why);
        res.prepare_payload();
        return res;
    };

    // Returns a not found response
    auto const not_found =
    [&req](beast::string_view target)
    {
        beast::http::response<gloer::net::http::ServerSession::request_body_t> res{beast::http::status::not_found, req.version()};
        res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(beast::http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "The resource '" + std::string(target) + "' was not found.";
        res.prepare_payload();
        return res;
    };

    // Returns a server error response
    auto const server_error =
    [&req](beast::string_view what)
    {
        beast::http::response<gloer::net::http::ServerSession::request_body_t> res{beast::http::status::internal_server_error, req.version()};
        res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(beast::http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "An error occurred: '" + std::string(what) + "'";
        res.prepare_payload();
        return res;
    };

    if(custom_response) {
      /// \todo support custom_response
      //return send(custom_response.value());
      return send(bad_request("Not allowed"));
    }

    // Make sure we can handle the method
    if( req.method() != beast::http::verb::get &&
        req.method() != beast::http::verb::head)
        return send(bad_request("Unknown HTTP-method"));

    // Request path must be absolute and not contain "..".
    if( req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != beast::string_view::npos)
        return send(bad_request("Illegal request-target"));

    // Build the path to the requested file
    std::string path = path_cat(doc_root, req.target());
    if(req.target().back() == '/')
        path.append("index.html");

    // Attempt to open the file
    beast::error_code ec;
    beast::http::file_body::value_type body;
    body.open(path.c_str(), beast::file_mode::scan, ec);

    // Handle the case where the file doesn't exist
    if(ec == boost::system::errc::no_such_file_or_directory)
        return send(not_found(req.target()));

    // Handle an unknown error
    if(ec)
        return send(server_error(ec.message()));

    // Cache the size since we need it after the move
    auto const size = body.size();

    const static gloer::net::http::MimeType mime_type;

    // Respond to HEAD request
    if(req.method() == beast::http::verb::head)
    {
        beast::http::response<beast::http::empty_body> res{beast::http::status::ok, req.version()};
        res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(beast::http::field::content_type, mime_type(path));
        res.content_length(size);
        res.keep_alive(req.keep_alive());
        return send(base::rvalue_cast(res));
    }

    // Respond to GET request
    beast::http::response<beast::http::file_body> res{
        std::piecewise_construct,
        std::make_tuple(base::rvalue_cast(body)),
        std::make_tuple(beast::http::status::ok, req.version())};
    res.set(beast::http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(beast::http::field::content_type, mime_type(path));
    res.content_length(size);
    res.keep_alive(req.keep_alive());
    return send(base::rvalue_cast(res));
}

} // namespace

namespace gloer {
namespace net {
namespace http {


// @note ::tcp::socket socket represents the local end of a connection between two peers
// NOTE: Following the base::rvalue_cast, the moved-from object is in the same state
// as if constructed using the basic_stream_socket(io_context&) constructor.
// boost.org/doc/libs/1_54_0/doc/html/boost_asio/reference/basic_stream_socket/basic_stream_socket/overload5.html
ServerSession::ServerSession(::boost::beast::limited_tcp_stream&& stream,
  gloer::net::http::DetectSession::buffer_t&& buffer,
  ::boost::asio::ssl::context& ctx,
  const http::SessionGUID& id)
    : SessionBase<http::SessionGUID>(id)
      //,
      , buffer_(/*base::rvalue_cast*/(buffer))
      , ctx_(ctx)
      , stream_(base::rvalue_cast(stream))
//      , request_deadline_{stream_.socket().get_executor(), (std::chrono::steady_clock::time_point::max)()}
{

  // TODO: stopped() == false
  // DCHECK(socket.get_executor().context().stopped() == false);

  DCHECK_GT(static_cast<std::string>(id).length(), 0);

  DCHECK_LT(static_cast<std::string>(id).length(), MAX_ID_LEN);

  // The policy object, which is default constructed, or
  // decay-copied upon construction, is attached to the stream
  // and may be accessed through the function `rate_policy`.
  //
  // Here we set individual rate limits for reading and writing
  stream_.rate_policy().read_limit(10000); // bytes per second
  stream_.rate_policy().write_limit(850000); // bytes per second
}

ServerSession::~ServerSession() {
  DCHECK(on_destruction);

  on_destruction(this); /// \note no shared_ptr in destructor

  /// \note don't call `close()` from desctructor, handle `close()` manually
  if(http_stream_valid_.load()) {
    DCHECK(!isOpen());
  }
}

void ServerSession::close() {
  DCHECK(isOpen());

  /// \note steam may be moved and it's memory may become corrupted
  if(http_stream_valid_.load()) {
    /// \note use ServerSession::do_eof() to close connection
    if(stream_.socket().is_open()) {
      beast::error_code ec;
      stream_.socket().close(ec);
    }
    DCHECK(!stream_.socket().is_open());
  }
}

/*void ServerSession::check_deadline_loop()
{
    // The deadline may have moved, so check it has really passed.
    if (request_deadline_.expiry() <= std::chrono::steady_clock::now())
    {
        // Close socket to cancel any outstanding operation.
        //beast::error_code ec;
        stream_.socket().close();

        // Sleep indefinitely until we're given a new deadline.
        request_deadline_.expires_at(
            std::chrono::steady_clock::time_point::max());
    }

    request_deadline_.async_wait(
        [this](beast::error_code ec)
        {
            check_deadline_loop();
            if(ec) {
                LOG(WARNING) << "error_code during check_deadline_loop: " << ec.message();
            }
        });
}*/

void ServerSession::on_session_fail(beast::error_code ec, char const* what) {
  DCHECK(http_stream_valid_.load());
  DCHECK(fail_handler);

  /// \note user can provide custom timeout handler e.t.c.
  if(!fail_handler(shared_from_this(), &ec, what)){
    return;
  }

  do_eof();

  if(isOpen()) {
    close();
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

  // Don't report these
  /*if (ec == ::boost::asio::error::operation_aborted
      || ec == ::websocket::error::closed) {
    return;
  }

  if (ec == beast::error::timeout) {
      LOG(INFO) << "|idle timeout when read"; //idle_timeout
      isExpired_ = true;
  }*/
}

size_t ServerSession::MAX_IN_MSG_SIZE_BYTE = 16 * 1024;
size_t ServerSession::MAX_OUT_MSG_SIZE_BYTE = 16 * 1024;

// Called by the base class
void ServerSession::do_eof()
{
  DCHECK(http_stream_valid_.load());

  const boost::asio::ip::tcp::endpoint remote_endpoint
    = beast::get_lowest_layer(stream_).socket().remote_endpoint();
  LOG(INFO) << "HTTP ServerSession::do_eof remote_endpoint: " << remote_endpoint;

  // Set the timeout.
  beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));

  // Send a TCP shutdown on asio thread
  if(stream_.socket().is_open())
  {
    LOG(INFO) << "shutdown http socket...";
    beast::error_code ec;
    /// \todo async_shutdown ? why only for SSL async_shutdown? https://github.com/OzzieIsaacs/winmerge-qt/blob/master/ext/boost_1_70_0/libs/beast/example/advanced/server-flex/advanced_server_flex.cpp#L764
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    if (ec) {
      /// \note don't call fail handlers here to prevent recursion, just log error
      LOG(WARNING) << "HTTP ServerSession shutdown: " << " : " << ec.message();
    }
  }

  // At this point the connection is closed gracefully
}

void ServerSession::on_shutdown(beast::error_code ec)
{
  if(ec) {
    return on_session_fail(ec, "shutdown");
  }

  // At this point the connection is closed gracefully
  DCHECK(http_stream_valid_.load());
  DCHECK(!stream_.socket().is_open());
}

// Start the asynchronous operation
void ServerSession::do_read() {
  //check_deadline_loop();

  LOG(INFO) << "HTTP session do_read";

  DCHECK(http_stream_valid_.load());

  const boost::asio::ip::tcp::endpoint remote_endpoint
    = beast::get_lowest_layer(stream_).socket().remote_endpoint();
  LOG(INFO) << "HTTP ServerSession::do_read remote_endpoint: " << remote_endpoint;

  // Construct a new parser for each message
  parser_.emplace();

  // Apply a reasonable limit to the allowed size
  // of the body in bytes to prevent abuse.
  parser_->body_limit(MAX_BUFFER_BYTES_SIZE);

  // Set the timeout.
  beast::get_lowest_layer(
      stream_).expires_after(std::chrono::seconds(30));

  // Read a request
  beast::http::async_read(
      stream_,
      buffer_,
      parser_->get(),
      beast::bind_front_handler(
          &http::ServerSession::on_read,
          shared_from_this()));
}

void ServerSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
  // This means they closed the connection
  if(ec == beast::http::error::end_of_stream) {
    do_eof();
    return;
  }

  // Handle the error, if any
  if(ec) {
    return on_session_fail(ec, "read");
  }

  DCHECK(http_stream_valid_.load());

  std::optional<beast::http::response<request_body_t>> http_resonse_on_fail
    = std::nullopt;

  // See if it is a WebSocket Upgrade
  if(websocket::is_upgrade(parser_->get()))
  {
      DCHECK(on_websocket_upgrade_);

      /// \note websocket upgrade may fail with some http response, like `permission denied`
      http_resonse_on_fail
        = on_websocket_upgrade_(shared_from_this(), &ec, bytes_transferred);

      if(http_resonse_on_fail) {
        /// \note failed to create ws stream, continue with http stream
        DCHECK(http_stream_valid_.load());
        LOG(WARNING) << "http_stream_valid_ = true";

        // TODO: do we need to close http session here if failed to update request???
      } else {
        /// \note `stream_` can be moved to websocket session from http session,
        // so we can't use it here anymore
        http_stream_valid_ = false;

        // Disable the timeout.
        // The websocket::stream uses its own timeout settings.
        beast::get_lowest_layer(stream_).expires_never();

        LOG(WARNING) << "http_stream_valid_ = false";
        return;
      }
  }

  /// \todo use send queue https://github.com/OzzieIsaacs/winmerge-qt/blob/master/ext/boost_1_70_0/libs/beast/example/advanced/server-flex/advanced_server_flex.cpp#L616
  // Send the response
  //
  // The following code requires generic
  // lambdas, available in C++14 and later.
  //
  DCHECK(http_stream_valid_.load());

  handle_request(
      "/",//state_->doc_root(),
      /*base::rvalue_cast*/(parser_->release()),
      http_resonse_on_fail,
      [this](auto&& response)
      {
          // The lifetime of the message has to extend
          // for the duration of the async operation so
          // we use a shared_ptr to manage it.
          using response_type = typename std::decay<decltype(response)>::type;
          auto sp = std::make_shared<response_type>(
            std::forward<decltype(response)>(response));
          // Write the response
          // NOTE: store `self` as separate variable due to ICE in gcc 7.3
          auto self = shared_from_this();
          beast::http::async_write(stream_, *sp,
              [self, sp](
                  beast::error_code ec, std::size_t bytes)
              {
                  self->on_write(ec, bytes, sp->need_eof());
              });
      });
}

void ServerSession::on_write(beast::error_code ec, std::size_t bytes_transferred, bool close) {
  DCHECK(http_stream_valid_.load());

  boost::ignore_unused(bytes_transferred);

  // Handle the error, if any
  if(ec) {
    return on_session_fail(ec, "write");
  }

  http::SessionGUID copyId = getId();

  if(close) {
    // This means we should close the connection, usually because
    // the response indicated the "Connection: close" semantic.
    do_eof();
    return;
  }

  // Read another request
  do_read();
}

void ServerSession::send(const std::shared_ptr<const std::string> shared_msg, bool is_binary) {
  LOG(WARNING) << "NOTIMPLEMENTED: HTTP ServerSession::send:";
  DCHECK(http_stream_valid_.load());
}

/**
 * @brief starts async writing to client
 *
 * @param message message passed to client
 */
void ServerSession::send(const std::string& message, bool is_binary) {
  LOG(WARNING) << "NOTIMPLEMENTED: HTTP ServerSession::send:";
  DCHECK(http_stream_valid_.load());
}

bool ServerSession::isOpen() /*const*/ {
  DCHECK(http_stream_valid_.load());
  return stream_.socket().is_open();
}

bool ServerSession::isExpired() /*const*/ {
  LOG(WARNING) << "NOTIMPLEMENTED: HTTP ServerSession::isExpired";
  DCHECK(http_stream_valid_.load());
  return true; // TODO
}

} // namespace http
} // namespace net
} // namespace gloer
