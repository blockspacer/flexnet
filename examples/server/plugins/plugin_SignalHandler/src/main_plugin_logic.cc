#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

#include <basis/log/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/status/statusor.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/strong_types/strong_alias.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <iostream>

namespace plugin {
namespace signal_handler {

MainPluginLogic::MainPluginLogic(
  const MainPluginInterface* pluginInterface)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , pluginInterface_{DCHECK_VALID_PTR_OR(pluginInterface)}
  , mainLoopRegistry_(
      ::backend::MainLoopRegistry::GetInstance())
  , mainLoopRunner_{
      ::base::MessageLoop::current()->task_runner()}
  , signalHandler_(
      REFERENCED(mainLoopRegistry_->registry()
        .ctx<::boost::asio::io_context>())
      // `bindToTaskRunner` re-routes callback to task runner
      , ::basis::bindToTaskRunner(
          FROM_HERE
          , ::base::BindOnce(
              &MainPluginLogic::handleSigQuit
              , weakSelf())
          , mainLoopRunner_)
    )
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

MainPluginLogic::~MainPluginLogic()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);
}

MainPluginLogic::VoidPromise
  MainPluginLogic::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::load()");

  return VoidPromise::CreateResolved(FROM_HERE);
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK(mainLoopRunner_);

  return VoidPromise::CreateResolved(FROM_HERE);
}

void MainPluginLogic::handleSigQuit()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::handleSigQuit()");

  LOG_CALL(DVLOG(99));

  DVLOG(9)
    << "got sigquit";

  DCHECK(mainLoopRunner_);
  (mainLoopRunner_)->PostTask(FROM_HERE
    , ::base::BindOnce(
        &MainPluginLogic::handleTerminationEvent
        , weakSelf()));
}

void MainPluginLogic::handleTerminationEvent()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // send termination event
  ::backend::AppState& appState =
    mainLoopRegistry_->registry()
      .ctx<::backend::AppState>();

  ::basis::Status result =
    appState.processStateChange(
      FROM_HERE
      , ::backend::AppState::TERMINATE);

  DCHECK(result.ok());
}

} // namespace signal_handler
} // namespace plugin
