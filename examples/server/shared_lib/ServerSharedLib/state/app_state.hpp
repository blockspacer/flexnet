#pragma once

#include <basis/ECS/safe_registry.hpp>
#include <basis/core/bitmask.hpp>

#include <base/rvalue_cast.h>
#include <base/callback.h> // IWYU pragma: keep
#include <base/macros.h>
#include <base/location.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>

#include <basis/checked_optional.hpp>
#include <basis/state_machine/unsafe_state_machine.hpp>
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/promise/promise.h>
#include <basis/promise/post_promise.h>
#include <basis/status/status.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/beast/core.hpp>

#include <atomic>
#include <memory>

namespace backend {

// MOTIVATION
//
// Prohibit invalid state transitions
// (like pausing from uninitialized state)
class AppState
{
public:
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
    // The application was running at some point,
    // but has been backgrounded to the
    // point where resources are invalid
    // and execution should be halted
    // until resumption.
    SUSPENDED = 3, /// \todo use it
    // Representation of a idle/terminal/shutdown state
    // with no resources.
    TERMINATED = 4,
    // Resources are invalid.
    FAILED = 5,
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
        AppState::State
        , AppState::Event
      >;

  using VoidPromise
    = ::base::Promise<void, ::base::NoReject>;

public:
  // USAGE
  //
  // // Warning: all callbacks must be used
  // // within the lifetime of the state machine.
  // StateMachineType::CallbackType okStateCallback =
  //   ::base::BindRepeating(
  //   []
  //   (Event event
  //    , State next_state
  //    , Event* recovery_event)
  //   {
  //     UNREFERENCED_PARAMETER(event);
  //     UNREFERENCED_PARAMETER(next_state);
  //     UNREFERENCED_PARAMETER(recovery_event);
  //     return ::basis::OkStatus(FROM_HERE);
  //   });
  // sm_->AddExitAction(UNINITIALIZED, okStateCallback);
  void AddExitAction(
   const State& state
   , const StateMachineType::CallbackType& callback) NO_EXCEPTION
  {
    sm_.AddExitAction(state, callback);
  }

  void AddEntryAction(
   const State& state
   , const StateMachineType::CallbackType& callback) NO_EXCEPTION
  {
    sm_.AddEntryAction(state, callback);
  }

  AppState(const State& initial_state = UNINITIALIZED)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  AppState(
    AppState&& other) = delete;

  ~AppState()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  SET_WEAK_SELF(AppState)

  MUST_USE_RETURN_VALUE
  State currentState()
  {
    return sm_.currentState();
  }

  /// \note Returns promise that can be resolved only once.
  /// If `Event` happens many times, than promise will NOT be resolved again
  /// (`GetRepeatingResolveCallback` will NOT `Run()`).
  MUST_USE_RETURN_VALUE
  VoidPromise promiseEntryOnce(
    const ::base::Location& from_here
    , const AppState::Event& processEvent) NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  /// \note Returns promise that can be resolved only once.
  /// If `Event` happens many times, than promise will NOT be resolved again
  /// (`GetRepeatingResolveCallback` will NOT `Run()`).
  MUST_USE_RETURN_VALUE
  VoidPromise promiseExitOnce(
    const ::base::Location& from_here
    , const AppState::Event& processEvent) NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  MUST_USE_RETURN_VALUE
  ::basis::Status processStateChange(
    const ::base::Location& from_here
    , const AppState::Event& processEvent) NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

private:

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
  StateMachineType::TransitionTable fillStateTransitionTable();

private:
  SET_WEAK_POINTERS(AppState);

  StateMachineType sm_;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(AppState);
};

} // namespace backend
