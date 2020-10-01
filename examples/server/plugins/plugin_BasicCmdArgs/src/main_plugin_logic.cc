#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

namespace plugin {
namespace basic_cmd_args {

// example: --version
static const char kVersionSwitch[]
  = "version";

// example: --help
static const char kHelpSwitch[]
  = "help";

MainPluginLogic::MainPluginLogic(
  const MainPluginInterface* pluginInterface)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , mainLoopRegistry_(
      ::backend::MainLoopRegistry::GetInstance())
  , mainLoopRunner_{
      base::MessageLoop::current()->task_runner()}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

MainPluginLogic::~MainPluginLogic()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);
}

void MainPluginLogic::handleCmd()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  base::CommandLine* cmdLine
    = base::CommandLine::ForCurrentProcess();
  DCHECK(cmdLine);

  if (cmdLine->HasSwitch(kVersionSwitch))
  {
    handleVersionCmd();
  }
  else if (cmdLine->HasSwitch(kHelpSwitch))
  {
    handleHelpCmd();
  }
}

void MainPluginLogic::handleVersionCmd()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  DVLOG(9)
    << "got "
    << kVersionSwitch
    << " console command";

  NOTIMPLEMENTED()
    << "TODO: --version";
}

void MainPluginLogic::handleHelpCmd()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  DVLOG(9)
    << "got "
    << kHelpSwitch
    << " console command";

  NOTIMPLEMENTED()
    << "TODO: --help";
}

MainPluginLogic::VoidPromise
  MainPluginLogic::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::load()");

  handleCmd();

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

} // namespace basic_cmd_args
} // namespace plugin
