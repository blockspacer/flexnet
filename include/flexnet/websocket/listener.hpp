#pragma once

#include "flexnet/util/access_verifier.hpp"
#include "flexnet/util/unsafe_state_machine.hpp"

#include <basis/ECS/asio_registry.hpp>
#include <basis/bitmask.h>

#include <base/rvalue_cast.h>
#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/location.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>

#include <basis/promise/promise.h>
#include <basis/promise/post_promise.h>
#include <basis/status/status.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep
#include <basis/ECS/simulation_registry.hpp>

#include <boost/asio/io_context.hpp>
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
//
// MOTIVATION
//
// 1. Uses memory pool
//    i.e. avoids slow memory allocations
//    We use ECS to imitate memory pool (using `UnusedTag` component),
//    so we can easily change type of data
//    that must use memory pool.
//
// 2. Uses ECS instead of callbacks to report result
//    i.e. result of `async_accept` e.t.c. stored in ECS entity.
//    That allows to process incoming data later
//    in `batches` (cache friendly).
//    Allows to customize logic dynamically
//    i.e. to discard all queued data for disconnected client
//    just add ECS system.
//
// 3. Avoids usage of `shared_ptr`.
//    You can store per-connection data in ECS entity
//    representing individual connection
//    i.e. allows to avoid custom memory management via RAII,
//    (destroy allocated data on destruction of ECS entity).
//
// 4. Provides extra thread-safety checks
//    i.e. uses `running_in_this_thread`,
//    `base/sequence_checker.h`, e.t.c.
//
// 5. Able to integrate with project-specific libs
//    i.e. use `base::Promise`, `base::RepeatingCallback`, e.t.c.
//
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
    = ::boost::asio::strand<::boost::asio::io_context::executor_type>;

  using StatusPromise
    = base::Promise<util::Status, base::NoReject>;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

#if DCHECK_IS_ON()
  enum State {
    UNINITIALIZED = 0,
    STARTED = 1,
    PAUSED = 2,
    TERMINATED = 2,
    FAILED = 3,
  };

  enum Event {
    START = 0,
    PAUSE = 1,
    TERMINATE = 2,
    FAULT = 3,
  };

  using StateMachineType =
      basis::UnsafeStateMachine<
        Listener::State
        , Listener::Event
      >;
#endif // DCHECK_IS_ON()

  // result of |acceptor_.async_accept(...)|
  // i.e. stores created socket
  struct AcceptConnectionResult {
    AcceptConnectionResult(
      ErrorCode&& _ec
      , SocketType&& _socket)
      : ec(base::rvalue_cast(_ec))
        , socket(base::rvalue_cast(_socket))
      {}

    AcceptConnectionResult(
      AcceptConnectionResult&& other)
      : AcceptConnectionResult(
          base::rvalue_cast(other.ec)
          , base::rvalue_cast(other.socket))
      {}

    /// \todo remove
    AcceptConnectionResult& operator=(
      AcceptConnectionResult&&) = default;

    ~AcceptConnectionResult()
    {
      LOG_CALL(DVLOG(99));
    }

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

  template <typename CallbackT>
  auto postTaskOnAcceptorStrand(
    const base::Location& from_here
    , CallbackT&& task)
  {
    // unable to `::boost::asio::post` on stopped ioc
    DCHECK(ioc_ && !ioc_->stopped());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , acceptorStrand_.ref_value(FROM_HERE)
      , std::forward<CallbackT>(task));
  }

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

  MUST_USE_RETURN_VALUE
    ::util::Status pause();

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
    return weak_this_.ref_value(FROM_HERE);
  }

private:
  MUST_USE_RETURN_VALUE
  ::util::Status processStateChange(
    const base::Location& from_here
    , const Listener::Event& processEvent)
  {
    const ::util::Status stateProcessed
      = sm_.ref_value(FROM_HERE).ProcessEvent(processEvent
        , FROM_HERE.ToString()
        , nullptr);
    CHECK(stateProcessed.ok())
      << "Failed to change state"
      << " using event "
      << processEvent
      << " in code "
      << from_here.ToString()
      << ". Current state: "
      << sm_.ref_value(FROM_HERE).CurrentState();
    return stateProcessed;
  }

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
  void setAcceptConnectionResult(
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

  // Defines all valid transitions for the state machine.
  // The transition table represents the following state diagram:
  // ASCII diagram generated using http://asciiflow.com/
  //      +-------------------+----------------+----------------+----------------+
  //      |                   ^                ^                ^                |
  //      |                   |     START      |                |                |
  //      |                   |   +---------+  |                |                |
  //      |                   |   |         |  |                |                |
  //      |                   +   v         +  |                +                v
  // UNINITIALIZED         STARTED         PAUSED          TERMINATED         FAILED
  //    +   +              ^  +  +          ^  +             ^   ^  ^
  //    |   |              |  |  |          |  |             |   |  |
  //    |   +---------------  +  +----------+  +-------------+   |  |
  //    |         START       |      PAUSE           TERMINATE   |  |
  //    |                     |                                  |  |
  //    |                     |                                  |  |
  //    |                     |                                  |  |
  //    |                     |                                  |  |
  //    |                     +----------------------------------+  |
  //    |                                   TERMINATE               |
  //    +-----------------------------------------------------------+
  //                              TERMINATE
  //
  MUST_USE_RETURN_VALUE
  StateMachineType::TransitionTable FillStateTransitionTable()
  {
    StateMachineType::TransitionTable sm_table_;

    //    [state][event] -> next state
    sm_table_[UNINITIALIZED][START] = STARTED;
    sm_table_[UNINITIALIZED][TERMINATE] = TERMINATED;
    sm_table_[UNINITIALIZED][FAULT] = FAILED;

    sm_table_[STARTED][PAUSE] = PAUSED;
    sm_table_[STARTED][TERMINATE] = TERMINATED;
    sm_table_[STARTED][FAULT] = FAILED;

    sm_table_[PAUSED][TERMINATE] = TERMINATED;
    sm_table_[PAUSED][FAULT] = FAILED;

    sm_table_[TERMINATED][TERMINATE] = TERMINATED;
    sm_table_[TERMINATED][FAULT] = FAILED;

    sm_table_[FAILED][FAULT] = FAILED;

    return sm_table_;
  }

  // Adds the entry and exit functions for each state.
  void AddStateCallbackFunctions()
  {
    // Warning: all callbacks must be used
    // within the lifetime of the state machine.

    StateMachineType::CallbackType okStateCallback =
      base::BindRepeating(
      []
      (Event event
       , State next_state
       , Event* recovery_event)
      {
        ignore_result(event);
        ignore_result(next_state);
        ignore_result(recovery_event);
        return ::util::OkStatus();
      });
    sm_.ref_value(FROM_HERE).AddExitAction(UNINITIALIZED, okStateCallback);
    sm_.ref_value(FROM_HERE).AddEntryAction(FAILED, okStateCallback);
  }

  ECS::Entity createTcpEntity(ECS::Registry& registry);

private:
  // Provides I/O functionality
  GLOBAL_THREAD_SAFE_LIFETIME()
  util::UnownedPtr<IoContext> ioc_;

  // acceptor will listen that address
  NOT_THREAD_SAFE_LIFETIME()
  EndpointType endpoint_;

  // used to create `per-connection entity`
  ECS::AsioRegistry& asioRegistry_;

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  basis::AccessVerifier<
    base::WeakPtrFactory<Listener>
    , basis::AccessVerifyPolicy::DebugOnly
  > weak_ptr_factory_;

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  basis::AccessVerifier<
    base::WeakPtr<Listener>
    , basis::AccessVerifyPolicy::DebugOnly
  > weak_this_;

  // modification of |acceptor_| guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  /// \note do not destruct |Listener| while |acceptorStrand_|
  /// has scheduled or execting tasks
  GLOBAL_THREAD_SAFE_LIFETIME()
  basis::AccessVerifier<
    const StrandType
    , basis::AccessVerifyPolicy::DebugOnly
  > acceptorStrand_;

  // The acceptor used to listen for incoming connections.
  basis::AccessVerifier<
    AcceptorType
    , basis::AccessVerifyPolicy::DebugOnly
  > acceptor_;

#if DCHECK_IS_ON()
  basis::AccessVerifier<
    StateMachineType
    , basis::AccessVerifyPolicy::DebugOnly
  > sm_;
#endif // DCHECK_IS_ON()

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
