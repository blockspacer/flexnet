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
      basis::UnsafeStateMachine<
        AppState::State
        , AppState::Event
      >;

public:
  AppState(const State& initial_state = UNINITIALIZED)
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

  AppState(
    AppState&& other) = delete;

  ~AppState()
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

  SET_WEAK_SELF(AppState)

  MUST_USE_RETURN_VALUE
  State currentState()
  {
    return sm_.currentState();
  }

  MUST_USE_RETURN_VALUE
  ::util::Status processStateChange(
    const base::Location& from_here
    , const AppState::Event& processEvent)
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

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
