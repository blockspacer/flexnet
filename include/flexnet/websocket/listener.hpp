#pragma once

#include "flexnet/util/checked_optional.hpp"
#include "flexnet/util/unsafe_state_machine.hpp"
#include "flexnet/util/lock_with_check.hpp"

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
#include <basis/unowned_ref.hpp> // IWYU pragma: keep
#include <basis/ECS/simulation_registry.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/beast/core.hpp>

#include <atomic>
#include <memory>

namespace util { template <class T> class UnownedRef; }

namespace base { struct NoReject; }

namespace ECS {

// Used to process component only once
// i.e. mark already processed components
// that can be re-used by memory pool.
CREATE_ECS_TAG(UnusedAcceptResultTag)

} // namespace ECS

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
// HOT-CODE PATH
//
// Use plain collbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
class Listener
{
public:
  /// \note Callback creates ECS entity used as `asio connection`,
  /// sets common components and performs checks
  /// (if entity was re-used from cache some components must be reset)
  using EntityAllocatorCb
    = base::RepeatingCallback<ECS::Entity()>;

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

  using ExecutorType
    // usually same as `::boost::asio::io_context::executor_type`
    = ::boost::asio::executor;

  using StrandType
    = ::boost::asio::strand<ExecutorType>;

  // Each created network entity will store
  // `::boost::asio::strand` as ECS component.
  using StrandComponent
    = base::Optional<StrandType>;

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
      , SocketType&& _socket
      , bool _need_close)
      : ec(base::rvalue_cast(_ec))
        , socket(base::rvalue_cast(_socket))
        , need_close(_need_close)
      {}

    AcceptConnectionResult(
      AcceptConnectionResult&& other)
      : AcceptConnectionResult(
          base::rvalue_cast(other.ec)
          , base::rvalue_cast(other.socket)
          , base::rvalue_cast(other.need_close))
      {}

    // Move assignment operator
    //
    // MOTIVATION
    //
    // To use type as ECS component
    // it must be `move-constructible` and `move-assignable`
    AcceptConnectionResult& operator=(
      AcceptConnectionResult&& rhs)
    {
      if (this != &rhs)
      {
        ec = base::rvalue_cast(rhs.ec);
        socket = base::rvalue_cast(rhs.socket);
        need_close = base::rvalue_cast(rhs.need_close);
      }

      return *this;
    }

    ~AcceptConnectionResult()
    {
      LOG_CALL(DVLOG(99));
    }

    ErrorCode ec;

    // socket created per each connection
    SocketType socket;

    // force socket closing
    // (before detection of SSL support)
    bool need_close;
  };

public:
  Listener(
   IoContext& ioc
    , EndpointType&& endpoint
    // ECS registry used to create `per-connection entity`
    , ECS::AsioRegistry& asioRegistry
    , EntityAllocatorCb entityAllocator);

  Listener(
    Listener&& other) = delete;

  ~Listener();

  // Start accepting incoming connections
  MUST_USE_RETURN_VALUE
  StatusPromise configureAndRun();

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnAcceptorStrand(
    const base::Location& from_here
    , CallbackT&& task
    , bool nestedPromise = false)
  {
    DCHECK_CUSTOM_THREAD_GUARD(acceptorStrand_);
    DCHECK_CUSTOM_THREAD_GUARD(ioc_);

    // unable to `::boost::asio::post` on stopped ioc
    DCHECK(!ioc_->stopped());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *acceptorStrand_
      , std::forward<CallbackT>(task)
      , nestedPromise);
  }

  // checks whether server is accepting new connections
  MUST_USE_RETURN_VALUE
  bool isAcceptorOpen() const;

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptorAsync();

  MUST_USE_RETURN_VALUE
    ::util::Status pause();

  // calls to |async_accept*| must be performed on same sequence
  // i.e. it is |acceptorStrand_.running_in_this_thread()|
  MUST_USE_RETURN_VALUE
  bool isAcceptingInThisThread() const NO_EXCEPTION;

  /// \note does not close alive sessions, just
  /// stops accepting incoming connections
  /// \note stopped acceptor may be continued via `async_accept`
  MUST_USE_RETURN_VALUE
    ::util::Status stopAcceptor();

  MUST_USE_RETURN_VALUE
  base::WeakPtr<Listener> weakSelf() const NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(weak_this_);

    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

private:
  // handles new connections
  void onAccept(util::UnownedPtr<StrandType> unownedPerConnectionStrand
                // `per-connection entity`
                , ECS::Entity tcp_entity_id
                , const ErrorCode& ec
                , SocketType&& socket);

  MUST_USE_RETURN_VALUE
  ::util::Status processStateChange(
    const base::Location& from_here
    , const Listener::Event& processEvent);

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
  void logFailure(
    const ErrorCode& ec, char const* what) RUN_ON_ANY_THREAD(logFailure);

  MUST_USE_RETURN_VALUE
    ::util::Status configureAcceptor();

  MUST_USE_RETURN_VALUE
    ::util::Status openAcceptor();

  void doAccept();

  MUST_USE_RETURN_VALUE
    ::util::Status configureAndRunAcceptor();

  // Defines all valid transitions for the state machine.
  // The transition table represents the following state diagram:
  /**
   * ASCII diagram generated using asciiflow.com
          +-------------------+----------------+----------------+----------------+
          ^                   ^                ^                ^                |
          |                   |     START      |                |                |
          |                   |   +---------+  |                |                |
          |                   |   |         |  |                |                |
          |                   +   v         +  |                +                v
     UNINITIALIZED         STARTED         PAUSED          TERMINATED         FAILED
        +   +              ^  +  +          ^  +             ^   ^  ^
        |   |              |  |  |          |  |             |   |  |
        |   +---------------  +  +----------+  +-------------+   |  |
        |         START       |      PAUSE           TERMINATE   |  |
        |                     |                                  |  |
        |                     |                                  |  |
        |                     |                                  |  |
        |                     |                                  |  |
        |                     +----------------------------------+  |
        |                                   TERMINATE               |
        +-----------------------------------------------------------+
                                  TERMINATE
  **/
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

private:
  // Provides I/O functionality
  const util::UnownedRef<IoContext> ioc_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(ioc_);

  // acceptor will listen that address
  const EndpointType endpoint_
    GUARDED_BY(acceptorStrand_);

  // used to create `per-connection entity`
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(asioRegistry_);

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<Listener> weak_ptr_factory_
    GUARDED_BY(sequence_checker_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  const base::WeakPtr<Listener> weak_this_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(weak_this_);

  // Modification of |acceptor_| must be guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  /// \note Do not destruct |Listener| while |acceptorStrand_|
  /// has scheduled or execting tasks.
  const basis::AnnotatedStrand<ExecutorType> acceptorStrand_
    SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
      acceptorStrand_
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that ioc not stopped
      // otherwise `::boost::asio::post` may fail.
      , base::BindRepeating(
        [](const util::UnownedRef<IoContext>& ioc) {
          return !ioc->stopped();
        }
        , CONST_REFERENCED(ioc_)
      ));

  // The acceptor used to listen for incoming connections.
  AcceptorType acceptor_
    GUARDED_BY(acceptorStrand_);

#if DCHECK_IS_ON()
  // MOTIVATION
  //
  // Prohibit invalid state transitions
  // (like pausing from uninitialized state)
  //
  // USAGE
  //
  // // Warning: all callbacks must be used
  // // within the lifetime of the state machine.
  // StateMachineType::CallbackType okStateCallback =
  //   base::BindRepeating(
  //   []
  //   (Event event
  //    , State next_state
  //    , Event* recovery_event)
  //   {
  //     ignore_result(event);
  //     ignore_result(next_state);
  //     ignore_result(recovery_event);
  //     return ::util::OkStatus();
  //   });
  // sm_->AddExitAction(UNINITIALIZED, okStateCallback);
  // sm_->AddEntryAction(FAILED, okStateCallback);
  StateMachineType sm_
    GUARDED_BY(acceptorStrand_);
#endif // DCHECK_IS_ON()

  EntityAllocatorCb entityAllocator_;

  CREATE_CUSTOM_THREAD_GUARD(logFailure);

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
