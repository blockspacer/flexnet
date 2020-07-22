#pragma once

#include <base/callback.h>
#include <base/macros.h>
#include <base/sequence_checker.h>

#include <basis/promise/promise.h>
#include <basis/status/status.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>

#include <memory>

namespace base { struct NoReject; }

namespace flexnet {

namespace ws {

/**
 * Accepts incoming connections and launches the sessions
 **/
class Listener
  : public std::enable_shared_from_this<Listener>
{
public:
  using EndpointType
    = ::boost::asio::ip::tcp::endpoint;

  using ErrorCode
    = ::boost::beast::error_code;

  using SocketType
    = ::boost::asio::ip::tcp::socket;

  using IoContext
    = ::boost::asio::io_context;

  using AcceptorType
    = ::boost::asio::ip::tcp::acceptor;

  using StrandType
    = boost::asio::io_service::strand;

  using AcceptedCallback
    = base::RepeatingCallback<
        void(ErrorCode* ec, SocketType* socket)
      >;

  using StatusPromise
    = base::Promise<util::Status, base::NoReject>;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

public:
  Listener(
    IoContext& ioc
    , const EndpointType& endpoint
    , AcceptedCallback&& acceptedCallback);

  ~Listener();

  // Report a failure
  void logFailure(ErrorCode ec, char const* what);

  // Start accepting incoming connections
  StatusPromise configureAndRun();

  /**
   * @brief checks whether server is accepting new connections
   */
  bool isAcceptorOpen() const;

  /**
   * @brief handles new connections and starts sessions
   */
  void onAccept(ErrorCode ec, SocketType socket);

  StatusPromise stopAcceptorAsync();

  bool isRunningInThisThread() const;

  /// \note does not close alive sessions, just
  /// stops accepting incoming connections
  /// \note stopped acceptor may be continued via `async_accept`
  ::util::Status stopAcceptor();

private:
  ::util::Status configureAcceptor();

  ::util::Status openAcceptor();

  void doAccept();

  ::util::Status configureAndRunAcceptor();

private:
  // The acceptor used to listen for incoming connections.
  AcceptorType acceptor_;

  IoContext& ioc_;

  EndpointType endpoint_;

  StrandType strand_;

  AcceptedCallback acceptedCallback_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
