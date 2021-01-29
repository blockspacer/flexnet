#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

namespace plugin {
namespace basic_terminal_commands {

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
  , consoleTerminalEventDispatcher_(
      REFERENCED(mainLoopRegistry_->registry()
        .ctx<::backend::ConsoleTerminalEventDispatcher>()))
  , mainLoopRunner_{
      ::base::MessageLoop::current()->task_runner()}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  consoleTerminalEventDispatcher_->sink<
    std::string
  >().connect<&MainPluginLogic::handleConsoleInput>(this);
}

MainPluginLogic::~MainPluginLogic()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  consoleTerminalEventDispatcher_->sink<
    std::string
  >().disconnect<&MainPluginLogic::handleConsoleInput>(this);
}

void MainPluginLogic::handleConsoleInput(
  const std::string& line)
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  if (line == "stop")
  {
    DVLOG(9)
      << "got `stop` console command";

    DCHECK(mainLoopRunner_);
    (mainLoopRunner_)->PostTask(FROM_HERE
      , ::base::BindOnce(
          &MainPluginLogic::handleTerminationEvent
          , weakSelf()
      ));
  }
}

void MainPluginLogic::handleTerminationEvent()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

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

MainPluginLogic::VoidPromise
  MainPluginLogic::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::load()");

  return
    VoidPromise::CreateResolved(FROM_HERE);
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK(mainLoopRunner_);

  return
    VoidPromise::CreateResolved(FROM_HERE);
}

} // namespace basic_terminal_commands
} // namespace plugin
