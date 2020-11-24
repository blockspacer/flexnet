#pragma once

#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"
#include "state/app_state.hpp"
#include "signal_handler/signal_handler.hpp"

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

#include <basis/scoped_sequence_context_var.hpp>
#include <basis/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>

namespace plugin {
namespace signal_handler {

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
  void handleSigQuit()
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  void handleTerminationEvent()
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

  /// \todo use `uv_signal_t` instead.
  /// \todo (ONLY _WIN32) SIGHUP: // Reload config/data
  /// \todo (ONLY _WIN32) SIGUSR1: // Saves state
  /// \todo (NOT _WIN32) SIGBREAK: // clean shutdown
  // Captures SIGINT and SIGTERM to perform a clean shutdown
  /// \note `boost::asio::signal_set` will NOT
  /// handle signals if ioc stopped
  plugin::signal_handler::SignalHandler signalHandler_
    GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(MainPluginLogic);
};

} // namespace signal_handler
} // namespace plugin
