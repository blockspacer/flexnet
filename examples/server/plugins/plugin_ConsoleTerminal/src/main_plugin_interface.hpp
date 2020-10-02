#pragma once

#include "main_plugin_logic.hpp"

#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"

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
namespace console_terminal {

// sets plugin metadata (configuration)
class MainPluginInterface
  final
  : public ::plugin::PluginInterface {
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

 public:
  SET_WEAK_SELF(MainPluginInterface)

  explicit MainPluginInterface(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  ~MainPluginInterface()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  std::string title() const override
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  std::string author() const override
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  std::string description() const override
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise load() override
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise unload() override
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  int consoleInputFreqMillisec() const NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

private:
  SET_WEAK_POINTERS(MainPluginInterface);

  std::string title_
    GUARDED_BY(sequence_checker_);

  std::string author_
    GUARDED_BY(sequence_checker_);

  std::string description_
    GUARDED_BY(sequence_checker_);

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    GUARDED_BY(sequence_checker_);

  // Use `base::Optional` because plugin can be unloaded i.e. `reset()`
  base::Optional<MainPluginLogic> mainPluginLogic_
    GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(MainPluginInterface);
};

} // namespace console_terminal
} // namespace plugin
