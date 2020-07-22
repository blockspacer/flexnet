#include "flexnet/websocket/listener.hpp" // IWYU pragma: associated

#include "flexnet/util/macros.hpp"

#include <base/bind.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/sequence_checker.h>

#include <basis/promise/post_promise.h>
#include <basis/status/status_macros.hpp>

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context_strand.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>

#include <boost/beast/websocket.hpp>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <iostream>
#include <memory>

namespace flexnet {
namespace ws {

Listener::Listener(
  IoContext& ioc
  , const EndpointType& endpoint
  , AcceptedCallback&& acceptedCallback)
  : acceptor_(ioc)
  , ioc_(ioc)
  , endpoint_(endpoint)
  , strand_(ioc_)
  , acceptedCallback_(std::move(acceptedCallback))
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

void Listener::logFailure(ErrorCode ec, char const* what)
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  // NOTE: If you got logFailure: accept: Too many open files
  // set ulimit -n 4096, see stackoverflow.com/a/8583083/10904212
  // Restart the accept operation if we got the connection_aborted error
  // and the enable_connection_aborted socket option is not set.
  if (ec == ::boost::asio::error::connection_aborted)
  {
    LOG_ERROR_CODE(LOG(WARNING),
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
    LOG_ERROR_CODE(LOG(WARNING),
      "Listener failed with"
      " stream_truncated error: ", what, ec);
    return;
  }

  if (ec == ::boost::asio::error::operation_aborted)
  {
    LOG_ERROR_CODE(LOG(WARNING),
      "Listener failed with"
      " operation_aborted error: ", what, ec);
    return;
  }

  if (ec == ::boost::beast::websocket::error::closed)
  {
    LOG_ERROR_CODE(LOG(WARNING),
      "Listener failed with"
      " websocket closed error: ", what, ec);
    return;
  }

  LOG_ERROR_CODE(LOG(WARNING),
    "Listener failed with"
    " error: ", what, ec);
}

::util::Status Listener::openAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  ErrorCode ec;

  VLOG(9)
    << "opening acceptor for "
    << endpoint_.address().to_string();

  acceptor_.open(endpoint_.protocol(), ec);
  if (ec)
  {
    logFailure(ec, "open");
    return MAKE_ERROR()
      << "Could not call open for acceptor";
  }

  if(!isAcceptorOpen()) {
    return MAKE_ERROR()
      << "Failed to open acceptor";
  }

  return ::util::OkStatus();
}

::util::Status Listener::configureAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  ErrorCode ec;

  // @see boost.org/doc/libs/1_61_0/doc/html/boost_asio/reference/basic_socket.html
  // Allow address reuse
  // NOTE: In windows, the option tcp::acceptor::reuse_address
  // is equivalent to calling setsockopt and specifying SO_REUSEADDR.
  // This specifically allows multiple sockets to be bound
  // to an address even if it is in use.
  // @see stackoverflow.com/a/7195105/10904212
  acceptor_.set_option(
    ::boost::asio::socket_base::reuse_address(true)
    , ec);
  if (ec)
  {
    logFailure(ec, "set_option");
    return MAKE_ERROR()
      << "Could not call set_option for acceptor";
  }

  // Bind to the server address
  acceptor_.bind(endpoint_, ec);
  if (ec)
  {
    logFailure(ec, "bind");
    return MAKE_ERROR()
      << "Could not call bind for acceptor";
  }

  VLOG(9)
    << "acceptor listening endpoint: "
    << endpoint_.address().to_string();

  acceptor_.listen(
    ::boost::asio::socket_base::max_listen_connections
    , ec);
  if (ec)
  {
    logFailure(ec, "listen");
    return MAKE_ERROR()
      << "Could not call listen for acceptor";
  }

  return ::util::OkStatus();
}

Listener::StatusPromise Listener::configureAndRun()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return base::PostPromiseAsio(FROM_HERE
    // Post our work to the strand, to prevent data race
    , strand_
    , base::BindOnce(
        &Listener::configureAndRunAcceptor,
        shared_from_this())
  );
}

::util::Status Listener::configureAndRunAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  RETURN_IF_ERROR(
    openAcceptor());

  RETURN_IF_ERROR(
    configureAcceptor());

  if(!isAcceptorOpen())
  {
    return MAKE_ERROR()
      << "Unable to run closed acceptor";
  }

  doAccept();

  return ::util::OkStatus();
}

void Listener::doAccept()
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  /**
   * I/O objects such as sockets and streams are not thread-safe.
   * For efficiency, networking adopts
   * a model of using threads without explicit locking
   * by requiring all access to I/O objects to be
   * performed within a strand.
   */
  acceptor_.async_accept(
    // new connection needs its own strand
    ::boost::asio::make_strand(ioc_),
    ::boost::beast::bind_front_handler(
        &Listener::onAccept,
        shared_from_this()));
}

::util::Status Listener::stopAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK(strand_.running_in_this_thread());

  /// \note we usually post `stopAcceptor()` using `boost::asio::post`,
  /// so need to check if acceptor was closed during delay
  /// added by `boost::asio::post`
  if(!isAcceptorOpen())
  {
    VLOG(9)
      << "unable to stop closed listener";
    return ::util::OkStatus();
  }

  ErrorCode ec;

  if (isAcceptorOpen())
  {
    VLOG(9)
      << "close acceptor...";

    acceptor_.cancel(ec);
    if (ec)
    {
      logFailure(ec, "acceptor_cancel");
      return MAKE_ERROR()
        << "Failed to call acceptor_cancel for acceptor";
    }

    /// \note does not close alive sessions, just
    /// stops accepting incoming connections
    /// \note stopped acceptor may be continued via `async_accept`
    acceptor_.close(ec);
    if (ec) {
      logFailure(ec, "acceptor_close");
      return MAKE_ERROR()
        << "Failed to call acceptor_close for acceptor";
    }

    // acceptor must be closed here without errors
    DCHECK(!isAcceptorOpen());
  }

  return ::util::OkStatus();
}

Listener::StatusPromise Listener::stopAcceptorAsync()
{
  LOG_CALL(VLOG(9));

  DCHECK(!strand_.running_in_this_thread());

  return base::PostPromiseAsio(FROM_HERE
    // Post our work to the strand, to prevent data race
    , strand_
    , base::BindOnce(
        &Listener::stopAcceptor,
        shared_from_this())
  );
}

void Listener::onAccept(ErrorCode ec, SocketType socket)
{
  LOG_CALL(VLOG(9));

  /// \todo unable to check strand here
  //DCHECK(strand_.running_in_this_thread());

  {
    const EndpointType& remote_endpoint
      = socket.remote_endpoint();
    VLOG(9)
        << "Listener accepted remote endpoint: "
        << remote_endpoint;
  }

  if (ec)
  {
    logFailure(ec, "accept");
  }

  /// \note we assume that |is_open|
  /// is thread-safe here
  /// (but not thread-safe in general)
  if (!acceptor_.is_open())
  {
    VLOG(9)
      << "unable to accept new connections"
      " on closed listener";

    return; // stop onAccept recursion
  }

  if (!ec)
  {
    DCHECK(socket.is_open());

    DCHECK(acceptedCallback_);
    /// \note usually calls |std::move(socket)|
    acceptedCallback_.Run(&ec, &socket);
  }

  // Accept another connection
  VoidPromise postResult
    = base::PostPromiseAsio(FROM_HERE
      // Post our work to the strand, to prevent data race
      , strand_
      , base::BindOnce(
          &Listener::doAccept,
          shared_from_this())
    );
  base::IgnoreResult(postResult);
}

Listener::~Listener()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG_CALL(VLOG(9));

  /// \note we assume that |is_open|
  /// is thread-safe in destructor
  /// (but not thread-safe in general)
  /// i.e. do not modify acceptor
  /// from any thread if you reached destructor
  DCHECK(!acceptor_.is_open());
}

bool Listener::isAcceptorOpen() const
{
  /// \note |is_open| is not thread-safe in general
  DCHECK(strand_.running_in_this_thread());

  return acceptor_.is_open();
}

bool Listener::isRunningInThisThread() const
{
  /// \note assumed to be thread-safe
  return strand_.running_in_this_thread();
}

} // namespace ws
} // namespace flexnet
