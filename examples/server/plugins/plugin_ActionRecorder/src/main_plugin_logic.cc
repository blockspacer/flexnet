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
namespace action_recorder {

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
      ::base::ThreadTaskRunnerHandle::Get()}
  , isSetActionCallback_{false}
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

  // posts task |base::SetRecordActionTaskRunner|
  // on provided task runner
  return setRecordActionTaskRunner(
    /// \note user actions will be recorded
    /// on |task_runner| from simulation
    /// i.e. on |FixedTaskLoopThread|
    mainLoopRunner_
  );
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK(mainLoopRunner_);

  return removeActionCallback();
}

void MainPluginLogic::onUserAction(
  const std::string& action_name)
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::onUserAction()");

  LOG_CALL(DVLOG(99));

  /// \todo create stats (metrics) for user actions
  LOG(INFO)
    << "happened action with name"
    << action_name;
}

void MainPluginLogic::setCustomCallback()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unsetCustomCallback()");

  LOG_CALL(DVLOG(99));

  // used by |base::RecordAction|
  // see https://chromium.googlesource.com/chromium/src.git/+/HEAD/tools/metrics/actions/README.md
  ::base::SetRecordActionTaskRunner(
    mainLoopRunner_);

  ::base::AddActionCallback(base::bindCheckedRepeating(
    DEBUG_BIND_CHECKS(
      PTR_CHECKER(this)
    )
    , &MainPluginLogic::onUserAction
    , ::base::Unretained(this)
  ));

  VLOG(9)
    << "added action callback...";
}

void MainPluginLogic::unsetCustomCallback()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unsetCustomCallback()");

  LOG_CALL(DVLOG(99));

  // see https://chromium.googlesource.com/chromium/src.git/+/HEAD/tools/metrics/actions/README.md
  ::base::RemoveActionCallback(base::bindCheckedRepeating(
    DEBUG_BIND_CHECKS(
      PTR_CHECKER(this)
    )
    , &MainPluginLogic::onUserAction
    , ::base::Unretained(this)
  ));

  VLOG(9)
    << "removed action callback...";
}

MainPluginLogic::VoidPromise MainPluginLogic::setRecordActionTaskRunner(
  scoped_refptr<::base::SingleThreadTaskRunner> task_runner)
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::setRecordActionTaskRunner()");

  LOG_CALL(DVLOG(99));

  DCHECK(task_runner);

  DCHECK(!isSetActionCallback_);
  isSetActionCallback_ = true;

  // thread that owns |task_runner_| must be running,
  // otherwise |PostTask| will do nothing
  MainPluginLogic::VoidPromise cbPromise
    = ::base::PostPromise(FROM_HERE
        , mainLoopRunner_.get()
        , ::base::BindOnce(
            &MainPluginLogic::setCustomCallback
            , weakSelf()
        )
    );

  return
    cbPromise;
}

MainPluginLogic::VoidPromise MainPluginLogic::removeActionCallback()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::removeActionCallback()");

  LOG_CALL(DVLOG(99));

  DVLOG(999)
    << "running"
    << FROM_HERE.ToString();

  DCHECK(isSetActionCallback_);
  isSetActionCallback_ = false;

  // thread that owns |task_runner_| must be running,
  // otherwise |PostTask| will do nothing
  MainPluginLogic::VoidPromise cbPromise
    = ::base::PostPromise(FROM_HERE
        , mainLoopRunner_.get()
        , ::base::BindOnce(
            &MainPluginLogic::unsetCustomCallback
            , weakSelf()
        )
    );

  return cbPromise;
}

} // namespace action_recorder
} // namespace plugin
