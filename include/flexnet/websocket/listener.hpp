#pragma once

#include "flexnet/ECS/asio_registry.hpp"

#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>

#include <basis/promise/promise.h>
#include <basis/status/status.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep
#include <basis/ECS/simulation_registry.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/beast/core.hpp>

#include <atomic>
#include <memory>

namespace util { template <class T> class UnownedRef; }

namespace base { struct NoReject; }

namespace flexnet {

namespace ws {

// Accepts incoming connections
class Listener
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
    = ::boost::asio::io_service::strand;

  using StatusPromise
    = base::Promise<util::Status, base::NoReject>;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

  // result of |acceptor_.async_accept(...)|
  // i.e. stores created socket
  struct AcceptNewConnectionResult {
    explicit AcceptNewConnectionResult(
      ErrorCode&& ec
      , SocketType&& socket)
      : ec(std::move(ec))
        , socket(std::move(socket))
      {}

    ErrorCode ec;
    // socket created per each connection
    SocketType socket;
  };

public:
  Listener(
    util::UnownedPtr<IoContext>&& ioc
    , EndpointType&& endpoint
    // ECS registry used to create `per-connection entity`
    , ECS::AsioRegistry& asioRegistry);

  ~Listener();

  // Start accepting incoming connections
  MUST_USE_RETURN_VALUE
  StatusPromise configureAndRun();

  // checks whether server is accepting new connections
  MUST_USE_RETURN_VALUE
  bool isAcceptorOpen() const;

  // handles new connections
  void onAccept(util::UnownedPtr<StrandType> unownedPerConnectionStrand
                // `per-connection entity`
                , ECS::Entity tcp_entity_id
                , const ErrorCode& ec
                , SocketType&& socket);

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptorAsync();

  // calls to |async_accept*| must be performed on same sequence
  // i.e. it is |acceptorStrand_.running_in_this_thread()|
  MUST_USE_RETURN_VALUE
  bool isAcceptingInThisThread() const noexcept;

  /// \note does not close alive sessions, just
  /// stops accepting incoming connections
  /// \note stopped acceptor may be continued via `async_accept`
  MUST_USE_RETURN_VALUE
    ::util::Status stopAcceptor();

  MUST_USE_RETURN_VALUE
  base::WeakPtr<Listener> weakSelf() const noexcept
  {
    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

private:
  // uses provided `per-connection entity`
  // to accept new connection
  // i.e. `per-connection data` will be stored
  // in `per-connection entity`
  void asyncAccept(
    util::UnownedPtr<StrandType> unownedPerConnectionStrand
    , ECS::Entity tcp_entity_id);

  void allocateTcpResourceAndAccept();

  // store result of |acceptor_.async_accept(...)|
  // in `per-connection entity`
  void setAcceptNewConnectionResult(
     // `per-connection entity`
      ECS::Entity tcp_entity_id
      , ErrorCode&& ec
      , SocketType&& socket);

  // Report a failure
  /// \note not thread-safe, so keep it for logging purposes only
  NOT_THREAD_SAFE_FUNCTION()
  void logFailure(
    const ErrorCode& ec, char const* what);

  MUST_USE_RETURN_VALUE
    ::util::Status configureAcceptor();

  MUST_USE_RETURN_VALUE
    ::util::Status openAcceptor();

  void doAccept();

  MUST_USE_RETURN_VALUE
    ::util::Status configureAndRunAcceptor();

private:
  // The acceptor used to listen for incoming connections.
  NOT_THREAD_SAFE_LIFETIME()
  AcceptorType acceptor_;

  // Provides I/O functionality
  GLOBAL_THREAD_SAFE_LIFETIME()
  util::UnownedPtr<IoContext> ioc_;

  // acceptor will listen that address
  NOT_THREAD_SAFE_LIFETIME()
  EndpointType endpoint_;

  ECS::AsioRegistry& asioRegistry_;

  // modification of |acceptor_| guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  /// \note do not destruct |Listener| while |acceptorStrand_|
  /// has scheduled or execting tasks
  NOT_THREAD_SAFE_LIFETIME()
  StrandType acceptorStrand_;

  //ECS::SimulationRegistry listenerRegistry_{}
  //LIVES_ON(acceptorStrand_);

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<
    Listener
    > weak_ptr_factory_
  LIVES_ON(sequence_checker_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  base::WeakPtr<Listener> weak_this_
  LIVES_ON(sequence_checker_);

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
