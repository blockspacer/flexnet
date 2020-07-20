#pragma once

#include <base/sequence_checker.h>

#include <boost/asio/io_context.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <memory>

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

public:
  Listener(
    IoContext& ioc
    , const EndpointType& endpoint);

  ~Listener();

  // Report a failure
  void onFail(ErrorCode ec, char const* what);

  // Start accepting incoming connections
  void run();

  /**
   * @brief checks whether server is accepting new connections
   */
  bool isAcceptorOpen() const;

  /**
   * @brief handles new connections and starts sessions
   */
  void onAccept(ErrorCode ec, SocketType socket);

  void stopAcceptorAsync();

  /// \note does not close alive sessions, just
  /// stops accepting incoming connections
  /// \note stopped acceptor may be continued via `async_accept`
  void stopAcceptor();

  std::function<
    void(ErrorCode* ec, SocketType* socket)
    > onAcceptedCallback_;

private:
  void configureAcceptor();

  void openAcceptor();

  void doAccept();

private:
  // The acceptor used to listen for incoming connections.
  AcceptorType acceptor_;

  IoContext& ioc_;

  EndpointType endpoint_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

bool Listener::isAcceptorOpen() const
{
  return acceptor_.is_open();
}

} // namespace ws
} // namespace flexnet
