#pragma once

#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"
#include "state/app_state.hpp"

#include <base/logging.h>
#include <base/cpu.h>
#include <base/macros.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/strings/string_util.h>
#include <base/trace_event/trace_event.h>
#include <base/strings/string_number_conversions.h>
#include <base/numerics/safe_conversions.h>

#include <base/at_exit.h>
#include <base/files/file_path.h>
#include <base/macros.h>
#include <base/bind.h>
#include <base/memory/scoped_refptr.h>
#include <base/message_loop/message_loop.h>
#include <base/single_thread_task_runner.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/observer_list_threadsafe.h>

#include <basis/log/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <string>
#include <vector>
#include <memory>

namespace plugin {
namespace action_recorder {

class MainPluginInterface;

// Performs plugin logic based on
// provided plugin interface (configuration)
class MainPluginLogic
{
 public:
  using VoidPromise
    = ::base::Promise<void, ::base::NoReject>;

  using StatusPromise
    = ::base::Promise<::basis::Status, ::base::NoReject>;

 public:
  SET_WEAK_SELF(MainPluginLogic)

  MainPluginLogic(
    const MainPluginInterface* pluginInterface)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  ~MainPluginLogic()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise load()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise unload()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

 private:
  // init with provided settings
  // posts task |base::SetRecordActionTaskRunner|
  // on provided task runner
  MUST_USE_RESULT
  VoidPromise setRecordActionTaskRunner(
    // |task_runner| will be used by |base::SetRecordActionTaskRunner|
    scoped_refptr<::base::SingleThreadTaskRunner> task_runner)
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  MUST_USE_RESULT
  VoidPromise removeActionCallback()
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  // used by |base::RecordAction|
  void onUserAction(
    const std::string& action_name)
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  void setCustomCallback()
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  void unsetCustomCallback()
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

 private:
  SET_WEAK_POINTERS(MainPluginLogic);

  ::basis::UnownedRef<
    const MainPluginInterface
  > pluginInterface_
      GUARDED_BY(sequence_checker_);

  ::basis::UnownedPtr<
    ::backend::MainLoopRegistry
  > mainLoopRegistry_
    GUARDED_BY(sequence_checker_);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<::base::SingleThreadTaskRunner> mainLoopRunner_
    GUARDED_BY(sequence_checker_);

  bool isSetActionCallback_
    GUARDED_BY(sequence_checker_);

  ::base::ActionCallback action_callback_
    GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(MainPluginLogic);
};

} // namespace action_recorder
} // namespace plugin
