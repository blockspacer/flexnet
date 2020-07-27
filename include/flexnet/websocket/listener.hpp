#pragma once

#include "flexnet/util/macros.hpp"
#include "flexnet/util/wrappers.hpp"
#include "flexnet/util/unowned_ptr.hpp"
#include "flexnet/util/unowned_ref.hpp"

#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/memory/weak_ptr.h>
#include <base/callback_list.h>

#include <basis/promise/promise.h>
#include <basis/status/status.hpp>
#include <basis/scoped_cleanup.hpp>

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
  // We assume that shared_ptr overhead is acceptable for |Listener|
  // i.e. |Listener| will not be created often
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
    = ::boost::asio::io_service::strand;

  using AllocateStrandCallback
    = base::RepeatingCallback<
        bool(
          util::UnownedRef<StrandType*> strand
          , util::UnownedRef<IoContext> ioc
          , util::UnownedPtr<Listener>&&)
      >;

  using DeallocateStrandCallback
    = base::RepeatingCallback<
        bool(
          StrandType*&& strand
          , util::UnownedPtr<Listener>&&)
      >;

  using AcceptedCallback
    = base::RepeatingCallback<
        void(
          util::UnownedPtr<Listener>&&
          , util::UnownedRef<ErrorCode> ec
          , util::UnownedRef<SocketType> socket
          , COPIED(util::UnownedPtr<StrandType> perConnectionStrand)
          // |scopedDeallocateStrand| can be used to control
          // lifetime of |perConnectionStrand|
          , util::ScopedCleanup& scopedDeallocateStrand)
      >;

  using StatusPromise
    = base::Promise<util::Status, base::NoReject>;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

public:
  Listener(
    util::UnownedPtr<IoContext>&& ioc
    , EndpointType&& endpoint
    , AllocateStrandCallback&& allocateStrandCallback
    , DeallocateStrandCallback&& deallocateStrandCallback);

  ~Listener();

  // Start accepting incoming connections
  MUST_USE_RETURN_VALUE
  StatusPromise configureAndRun();

  /**
   * @brief checks whether server is accepting new connections
   */
  MUST_USE_RETURN_VALUE
  bool isAcceptorOpen() const;

  /**
   * @brief handles new connections and starts sessions
   */
  void onAccept(ErrorCode ec
    , SocketType socket
    , StrandType* perConnectionStrand);

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

  void registerAcceptedCallback(const AcceptedCallback& cb);

  MUST_USE_RETURN_VALUE
  base::WeakPtr<Listener> weakSelf() const noexcept
  {
    return weak_this_;
  }

private:
  // Report a failure
  /// \note not thread-safe, so keep it for logging purposes only
  void CAUTION_NOT_THREAD_SAFE(logFailure)
    (ErrorCode ec, char const* what);

  MUST_USE_RETURN_VALUE
  ::util::Status configureAcceptor();

  MUST_USE_RETURN_VALUE
  ::util::Status openAcceptor();

  void doAccept();

  MUST_USE_RETURN_VALUE
  ::util::Status configureAndRunAcceptor();

private:
  // The acceptor used to listen for incoming connections.
  AcceptorType acceptor_;

  // Provides I/O functionality
  util::UnownedPtr<IoContext> ioc_;

  // acceptor will listen that address
  EndpointType endpoint_;

  // modification of |acceptor_| guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  StrandType acceptorStrand_;

  /// \note take care of thread-safety
  /// i.e. change |acceptedCallback_| only when acceptor stopped
  CAUTION_NOT_THREAD_SAFE(AcceptedCallback) acceptedCallback_;

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<
      Listener
    > weak_ptr_factory_;

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  base::WeakPtr<Listener> weak_this_;

  // unlike |isAcceptorOpen()| it can be used from any sequence,
  // but it is approximation that stores state
  // based on results of previous API calls
  // i.e. it may be NOT same as |acceptor_.is_open()|
  std::atomic<bool> assume_is_accepting_{false};

  /// \note allow API users to use custom allocators
  /// (like memory pool) to increase performance
  /// \note usually it is same as
  /// StrandType* perConnectionStrand
  ///   = new (std::nothrow) StrandType(ioc_);
  AllocateStrandCallback allocateStrandCallback_;

  /// \note allow API users to use custom allocators
  /// (like memory pool) to increase performance
  DeallocateStrandCallback deallocateStrandCallback_;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
