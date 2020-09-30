#pragma once

#include <basis/ECS/asio_registry.hpp>
#include <basis/bitmask.h>

#include <base/rvalue_cast.h>
#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/location.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>

#include <basis/checked_optional.hpp>
#include <basis/state_machine/unsafe_state_machine.hpp>
#include <basis/lock_with_check.hpp>
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
// Use plain callbacks (do not use `base::Promise` etc.)
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
    // A possible initial state where the application can be running,
    // loading data, and so on, but is not visible to the user.
    UNINITIALIZED = 0,
    // The state where the application is running in the foreground,
    // fully visible, with all necessary resources available.
    // A possible initial state, where loading happens
    // while in the foreground.
    STARTED = 1,
    // The application is expected to be able to move back into
    // the Started state very quickly
    PAUSED = 2,
    // Representation of a idle/terminal/shutdown state
    // with no resources.
    TERMINATED = 3,
    // Resources are invalid.
    FAILED = 4,
  };

  enum Event {
    START = 0,
    PAUSE = 1,
    SUSPEND = 2,
    TERMINATE = 3,
    FAULT = 4,
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
    , base::IsNestedPromise isNestedPromise = base::IsNestedPromise())
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(acceptorStrand_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(ioc_));

    // unable to `::boost::asio::post` on stopped ioc
    DCHECK(!ioc_->stopped());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *acceptorStrand_
      , std::forward<CallbackT>(task)
      , isNestedPromise);
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

  SET_WEAK_SELF(Listener)

private:
  // handles new connections
  /// \note `async_accept` will be called
  /// not only on new connection,
  /// but also when `acceptor_` stops
  /// while `async_accept` is awaiting.
  /// i.e. `onAccept` will be called anyway
  /// and we can use it to free allocated resources.
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

  void allocateTcpResourceAndAccept()
    RUN_ON(&asioRegistry_->strand);

  // store result of |acceptor_.async_accept(...)|
  // in `per-connection entity`
  void setAcceptConnectionResult(
    // `per-connection entity`
    ECS::Entity tcp_entity_id
    , ErrorCode&& ec
    , SocketType&& socket)
    RUN_ON(&asioRegistry_->strand);

  // Report a failure
  /// \note not thread-safe, so keep it for logging purposes only
  void logFailure(const ErrorCode& ec, char const* what)
    RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(FUNC_GUARD(logFailure));

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
  StateMachineType::TransitionTable fillStateTransitionTable()
  {
    StateMachineType::TransitionTable sm_table_;

    // [state][event] -> next state
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
  SET_WEAK_POINTERS(Listener);

  // Provides I/O functionality
  const util::UnownedRef<IoContext> ioc_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(ioc_));

  // acceptor will listen that address
  const EndpointType endpoint_
    GUARDED_BY(acceptorStrand_);

  // used to create `per-connection entity`
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(asioRegistry_));

  // Modification of |acceptor_| must be guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  /// \note Do not destruct |Listener| while |acceptorStrand_|
  /// has scheduled or execting tasks.
  const basis::AnnotatedStrand<ExecutorType> acceptorStrand_
    SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
      MEMBER_GUARD(acceptorStrand_)
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that ioc not stopped
      // otherwise `::boost::asio::post` may fail.
      , base::BindRepeating(
          []
          (Listener* self) -> bool {
            DCHECK_THREAD_GUARD_SCOPE(self->MEMBER_GUARD(ioc_));
            return !self->ioc_->stopped();
          }
          , base::Unretained(this)
        )
    );

  // The acceptor used to listen for incoming connections.
  AcceptorType acceptor_
    GUARDED_BY(acceptorStrand_);

#if DCHECK_IS_ON()
  // MOTIVATION
  //
  // Prohibit invalid state transitions
  // (like pausing from uninitialized state)
  StateMachineType sm_
    GUARDED_BY(acceptorStrand_);
#endif // DCHECK_IS_ON()

  EntityAllocatorCb entityAllocator_;

  /// \note can be called from any thread
  CREATE_CUSTOM_THREAD_GUARD(FUNC_GUARD(logFailure));

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet
