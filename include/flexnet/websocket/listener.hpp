#pragma once

#include <basis/ECS/safe_registry.hpp>
#include <basis/core/bitmask.hpp>

#include <base/rvalue_cast.h>
#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/location.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>

#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
#include <basis/state_machine/unsafe_state_machine.hpp>
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/promise/promise.h>
#include <basis/promise/post_promise.h>
#include <basis/status/status.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>
#include <basis/fail_point/fail_point.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/beast/core.hpp>

#include <atomic>
#include <memory>

namespace util { template <class T> class UnownedPtr; }

namespace base { struct NoReject; }

namespace ECS {

// Used to process component only once
// i.e. mark already processed components
// that can be re-used by memory pool.
CREATE_ECS_TAG(UnusedAcceptResultTag)

} // namespace ECS

namespace flexnet {

namespace ws {

STRONG_FAIL_POINT(FP_ConnectionAborted);

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
    = ::base::RepeatingCallback<ECS::Entity()>;

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
    = ::base::Optional<StrandType>;

  using StatusPromise
    = ::base::Promise<basis::Status, ::base::NoReject>;

  using VoidPromise
    = ::base::Promise<void, ::base::NoReject>;

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
      ::basis::UnsafeStateMachine<
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
      : ec(RVALUE_CAST(_ec))
        , socket(RVALUE_CAST(_socket))
        , need_close(_need_close)
      {}

    AcceptConnectionResult(
      AcceptConnectionResult&& other)
      : AcceptConnectionResult(
          RVALUE_CAST(other.ec)
          , RVALUE_CAST(other.socket)
          , RVALUE_CAST(other.need_close))
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
        ec = RVALUE_CAST(rhs.ec);
        socket = RVALUE_CAST(rhs.socket);
        need_close = RVALUE_CAST(rhs.need_close);
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
    , ECS::SafeRegistry& registry
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
    const ::base::Location& from_here
    , CallbackT&& task
    , ::base::IsNestedPromise isNestedPromise = ::base::IsNestedPromise())
  {
    DCHECK_MEMBER_GUARD(acceptorStrand_);

    // unable to `::boost::asio::post` on stopped ioc
    DCHECK(!ioc_.stopped());
    return ::base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *acceptorStrand_
      , FORWARD(task)
      , isNestedPromise);
  }

  // checks whether server is accepting new connections
  MUST_USE_RETURN_VALUE
  bool isAcceptorOpen() const;

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptorAsync();

  MUST_USE_RETURN_VALUE
    ::basis::Status pause();

  // calls to |async_accept*| must be performed on same sequence
  // i.e. it is |acceptorStrand_.running_in_this_thread()|
  MUST_USE_RETURN_VALUE
  bool isAcceptingInThisThread() const NO_EXCEPTION;

  /// \note does not close alive sessions, just
  /// stops accepting incoming connections
  /// \note stopped acceptor may be continued via `async_accept`
  MUST_USE_RETURN_VALUE
    ::basis::Status stopAcceptor();

  SET_WEAK_SELF(Listener)

private:
  // handles new connections
  /// \note `async_accept` will be called
  /// not only on new connection,
  /// but also when `acceptor_` stops
  /// while `async_accept` is awaiting.
  /// i.e. `onAccept` will be called anyway
  /// and we can use it to free allocated resources.
  void onAccept(basis::UnownedPtr<StrandType> perConnectionStrand
                // `per-connection entity`
                , ECS::Entity tcp_entity_id
                , const ErrorCode& ec
                , SocketType&& socket);

#if DCHECK_IS_ON()
  MUST_USE_RETURN_VALUE
  ::basis::Status processStateChange(
    const ::base::Location& from_here
    , const Listener::Event& processEvent);
#endif // DCHECK_IS_ON()

  // uses provided `per-connection entity`
  // to accept new connection
  // i.e. `per-connection data` will be stored
  // in `per-connection entity`
  void asyncAccept(
    ::basis::UnownedPtr<StrandType> perConnectionStrand
    , ECS::Entity tcp_entity_id);

  void allocateTcpResourceAndAccept()
    PRIVATE_METHOD_RUN_ON(registry_);

  // store result of |acceptor_.async_accept(...)|
  // in `per-connection entity`
  void setAcceptConnectionResult(
    // `per-connection entity`
    ECS::Entity tcp_entity_id
    , ErrorCode&& ec
    , SocketType&& socket)
    PRIVATE_METHOD_RUN_ON(registry_);

  // Report a failure
  /// \note not thread-safe, so keep it for logging purposes only
  void logFailure(const ErrorCode& ec, char const* what);

  MUST_USE_RETURN_VALUE
    ::basis::Status configureAcceptor();

  MUST_USE_RETURN_VALUE
    ::basis::Status openAcceptor();

  void doAccept();

  MUST_USE_RETURN_VALUE
    ::basis::Status configureAndRunAcceptor();

#if DCHECK_IS_ON()
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
#endif // DCHECK_IS_ON()

  MUST_USE_RETURN_VALUE
  bool isIocRunning() const NO_EXCEPTION
  {
    return !ioc_.stopped();
  }

private:
  SET_WEAK_POINTERS(Listener);

  // Provides I/O functionality
  IoContext& ioc_;

  // acceptor will listen that address
  const EndpointType endpoint_
    GUARDED_BY(acceptorStrand_);

  // used to create `per-connection entity`
  ECS::SafeRegistry& registry_;

  // Modification of |acceptor_| must be guarded by |acceptorStrand_|
  // i.e. acceptor_.open(), acceptor_.close(), etc.
  /// \note Do not destruct |Listener| while |acceptorStrand_|
  /// has scheduled or execting tasks.
  const ::basis::AnnotatedStrand<ExecutorType> acceptorStrand_
    GUARD_MEMBER_WITH_CHECK(
      acceptorStrand_
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that ioc not stopped
      // otherwise `::boost::asio::post` may fail.
      , ::base::bindCheckedRepeating(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &Listener::isIocRunning
          , ::base::Unretained(this)
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

  EntityAllocatorCb entityAllocator_
    GUARDED_BY(registry_);

  // Warn about large ECS registry.
  size_t warnBigRegistrySize_
    GUARDED_BY(registry_);

  // Max. log frequency in millis.
  // See `warnBigRegistrySize_`.
  int warnBigRegistryFreqMs_
    GUARDED_BY(registry_);

  FP_ConnectionAborted* FAIL_POINT(onAborted_) = nullptr;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(Listener);
};

} // namespace ws
} // namespace flexnet

ECS_DECLARE_METATYPE(UnusedAcceptResultTag)

ECS_DECLARE_METATYPE(::base::Optional<::flexnet::ws::Listener::AcceptConnectionResult>)

ECS_DECLARE_METATYPE(::base::Optional<::flexnet::ws::Listener::StrandComponent>)
