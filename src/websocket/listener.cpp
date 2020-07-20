#include "flexnet/websocket/listener.hpp" // IWYU pragma: associated

#include <base/location.h>
#include <base/sequence_checker.h>
#include <base/logging.h>

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/detail/impl/reactive_socket_service_base.ipp>
#include <boost/asio/detail/impl/service_registry.hpp>
#include <boost/asio/detail/impl/strand_executor_service.ipp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/impl/execution_context.hpp>
#include <boost/asio/impl/executor.hpp>
#include <boost/asio/impl/io_context.hpp>
#include <boost/asio/impl/post.hpp>
#include <boost/asio/impl/system_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/detail/impl/endpoint.ipp>
#include <boost/asio/ip/impl/address.ipp>
#include <boost/asio/ip/impl/basic_endpoint.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/strand.hpp>

#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/detail/bind_handler.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/impl/error.ipp>

#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>

#define LOG_ERROR(LOG_STREAM, description, what, ec) \
  LOG_STREAM  \
    << description  \
    << what  \
    << ": "  \
    << ec.message();

#define LOG_CALL(LOG_STREAM) \
  LOG_STREAM \
    << "called " \
    << FROM_HERE.ToString();

namespace flexnet {
namespace ws {

Listener::Listener(
  IoContext& ioc,
  const EndpointType& endpoint)
  : acceptor_(ioc)
  , ioc_(ioc)
  , endpoint_(endpoint)
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  openAcceptor();

  configureAcceptor();
}

void Listener::onFail(ErrorCode ec, char const* what)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // NOTE: If you got onFail: accept: Too many open files
  // set ulimit -n 4096, see stackoverflow.com/a/8583083/10904212
  // Restart the accept operation if we got the connection_aborted error
  // and the enable_connection_aborted socket option is not set.
  if (ec == ::boost::asio::error::connection_aborted)
  {
    LOG_ERROR(LOG(WARNING),
              "Listener failed with"
              " connection_aborted error: ", what, ec);
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
  if(ec == ::boost::asio::ssl::error::stream_truncated)
  {
    LOG_ERROR(LOG(WARNING),
              "Listener failed with"
              " stream_truncated error: ", what, ec);
    return;
  }

  if (ec == ::boost::asio::error::operation_aborted)
  {
    LOG_ERROR(LOG(WARNING),
              "Listener failed with"
              " operation_aborted error: ", what, ec);
    return;
  }

  if (ec == ::boost::beast::websocket::error::closed)
  {
    LOG_ERROR(LOG(WARNING),
              "Listener failed with"
              " websocket closed error: ", what, ec);
    return;
  }

  LOG_ERROR(LOG(WARNING),
            "Listener failed with"
            " error: ", what, ec);
}

void Listener::openAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ErrorCode ec;

  VLOG(9)
    << "opening acceptor for "
    << endpoint_.address().to_string();

  acceptor_.open(endpoint_.protocol(), ec);
  if (ec)
  {
    onFail(ec, "open");
    return;
  }

  // sanity check
  DCHECK(isAcceptorOpen());
}

void Listener::configureAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ErrorCode ec;

  // @see boost.org/doc/libs/1_61_0/doc/html/boost_asio/reference/basic_socket.html
  // Allow address reuse
  // NOTE: In windows, the option tcp::acceptor::reuse_address
  // is equivalent to calling setsockopt and specifying SO_REUSEADDR.
  // This specifically allows multiple sockets to be bound
  // to an address even if it is in use.
  // @see stackoverflow.com/a/7195105/10904212
  acceptor_.set_option(::boost::asio::socket_base::reuse_address(
                         true), ec);
  if (ec)
  {
    onFail(ec, "set_option");
    return;
  }

  // Bind to the server address
  acceptor_.bind(endpoint_, ec);
  if (ec)
  {
    onFail(ec, "bind");
    return;
  }

  VLOG(9)
    << "acceptor listening endpoint: "
    << endpoint_.address().to_string();

  acceptor_.listen(
    ::boost::asio::socket_base::max_listen_connections, ec);
  if (ec)
  {
    onFail(ec, "listen");
    return;
  }
}

void Listener::run()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(isAcceptorOpen());

  doAccept();
}

void Listener::doAccept()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  /**
   * I/O objects such as sockets and streams are not thread-safe. For efficiency, networking adopts
   * a model of using threads without explicit locking by requiring all access to I/O objects to be
   * performed within a strand.
   */
  acceptor_.async_accept(
    // The new connection gets its own strand
    ::boost::asio::make_strand(ioc_),
    ::boost::beast::bind_front_handler(
      &Listener::onAccept,
      shared_from_this()));
}

void Listener::stopAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  /// \note we usually post `stopAcceptor()` using `boost::asio::post`,
  /// so need to check if acceptor was closed during delay
  /// added by `boost::asio::post`
  if(!isAcceptorOpen())
  {
    VLOG(9)
      << "unable to stop closed listener";
    return;
  }

  try {
    ErrorCode ec;

    if (acceptor_.is_open())
    {
      VLOG(9)
        << "close acceptor...";

      acceptor_.cancel(ec);
      if (ec)
      {
        onFail(ec, "acceptor_cancel");
      }

      /// \note does not close alive sessions, just
      /// stops accepting incoming connections
      /// \note stopped acceptor may be continued via `async_accept`
      acceptor_.close(ec);
      if (ec) {
        onFail(ec, "acceptor_close");
      }
    }
  } catch (const boost::system::system_error& ex) {
    LOG(WARNING)
      << "Listener::stop: exception: " << ex.what();
  }
}

void Listener::stopAcceptorAsync()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Post our work to the strand, to prevent data race
  boost::asio::post(
    acceptor_.get_executor(),
    ::boost::beast::bind_front_handler(
      &Listener::stopAcceptor,
      shared_from_this()));
}

void Listener::onAccept(ErrorCode ec, SocketType socket)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  {
    const EndpointType& remote_endpoint
      = socket.remote_endpoint();
    VLOG(9)
        << "Listener accepted remote endpoint: "
        << remote_endpoint;
  }

  if (!isAcceptorOpen())
  {
    VLOG(9)
      << "unable to accept new connections"
      " on closed listener";
    return; // stop onAccept recursion
  }

  if (ec)
  {
    onFail(ec, "accept");
  }
  else
  {
    DCHECK(socket.is_open());

    DCHECK(onAcceptedCallback_);
    onAcceptedCallback_(&ec, &socket);
  }

  // Accept another connection
  doAccept();
}

Listener::~Listener()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG_CALL(VLOG(9));

  DCHECK(!isAcceptorOpen());
}

} // namespace ws
} // namespace flexnet
