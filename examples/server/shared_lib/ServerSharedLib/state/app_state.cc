#include "state/app_state.hpp" // IWYU pragma: associated
#include "flexnet/ECS/components/tcp_connection.hpp"

#include <base/rvalue_cast.h>
#include <base/bind.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/guid.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>

#include <basis/ECS/tags.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/promise/post_promise.h>
#include <basis/core/scoped_cleanup.hpp>
#include <basis/status/status_macros.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp> // IWYU pragma: keep
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context_strand.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/beast/websocket.hpp>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>

namespace backend {

AppState::AppState(const State& initial_state)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , sm_(initial_state, fillStateTransitionTable())
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

basis::Status AppState::processStateChange(
  const ::base::Location &from_here
  , const AppState::Event &processEvent) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  const ::basis::Status stateProcessed
      = sm_.ProcessEvent(processEvent
                         , FROM_HERE.ToString()
                         , nullptr);
  if(!stateProcessed.ok())
  {
    LOG(WARNING)
      << "Failed to change state"
      << " using event "
      << processEvent
      << " in code "
      << from_here.ToString()
      << ". Current state: "
      << sm_.currentState();
    DCHECK(false);
  }
  return stateProcessed;
}

AppState::VoidPromise
  AppState::promiseEntryOnce(
    const ::base::Location& from_here
    , const AppState::Event& processEvent) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  ::base::ManualPromiseResolver<void, ::base::NoReject>
    eventOnceResolver(FROM_HERE);

  AddEntryAction(AppState::TERMINATED, ::base::BindRepeating(
    []
    (base::OnceClosure&& resolveCb
     , AppState::Event event
     , AppState::State next_state
     , AppState::Event* recovery_event)
    {
      LOG_CALL(DVLOG(99));

      UNREFERENCED_PARAMETER(event);
      UNREFERENCED_PARAMETER(next_state);
      UNREFERENCED_PARAMETER(recovery_event);

      /// \note `AddEntryAction` will add repeating callback,
      /// but we want to execute it only once
      if(resolveCb) {
        RVALUE_CAST(resolveCb).Run();
      }

      return ::basis::OkStatus(FROM_HERE);
    }
    /// \note A ::base::RepeatingCallback can only be run once
    /// if arguments were bound with ::base::Passed().
    , ::base::Passed(eventOnceResolver.GetRepeatingResolveCallback())
  ));

  return eventOnceResolver.promise();
}

AppState::VoidPromise
  AppState::promiseExitOnce(
    const ::base::Location& from_here
    , const AppState::Event& processEvent) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  ::base::ManualPromiseResolver<void, ::base::NoReject>
    eventOnceResolver(FROM_HERE);

  AddEntryAction(AppState::TERMINATED, ::base::BindRepeating(
    []
    (base::OnceClosure&& resolveCb
     , AppState::Event event
     , AppState::State next_state
     , AppState::Event* recovery_event)
    {
      LOG_CALL(DVLOG(99));

      UNREFERENCED_PARAMETER(event);
      UNREFERENCED_PARAMETER(next_state);
      UNREFERENCED_PARAMETER(recovery_event);

      /// \note `AddEntryAction` will add repeating callback,
      /// but we want to execute it only once
      if(resolveCb) {
        RVALUE_CAST(resolveCb).Run();
      }

      return ::basis::OkStatus(FROM_HERE);
    }
    /// \note A ::base::RepeatingCallback can only be run once
    /// if arguments were bound with ::base::Passed().
    , ::base::Passed(eventOnceResolver.GetRepeatingResolveCallback())
  ));

  return eventOnceResolver.promise();
}

AppState::StateMachineType::TransitionTable
  AppState::fillStateTransitionTable()
{
  LOG_CALL(DVLOG(99));

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

AppState::~AppState()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  /// \note we expect that API user will call
  /// `close` for acceptor before acceptor destructon
  DCHECK(
    sm_.currentState() == AppState::UNINITIALIZED
    || sm_.currentState() == AppState::TERMINATED);
}

} // namespace backend
