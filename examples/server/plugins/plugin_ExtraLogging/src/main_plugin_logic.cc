#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

#include <basis/scoped_sequence_context_var.hpp>
#include <basis/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/strong_types/strong_alias.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <iostream>

namespace plugin {
namespace extra_logging {

static void checkNumOfCores(
  const size_t minCores)
{
  unsigned int coresTotal
    = std::thread::hardware_concurrency();

  LOG(INFO)
    << "Number of cores: "
    << coresTotal;

  if (coresTotal < minCores)
  {
    LOG(INFO)
      << "Too low number of cores!"
         " Prefer servers with at least "
      << minCores
      << " cores";
  }
}

static void displayMachineInfo()
{
  ::base::CPU cpu;

  LOG(INFO)
    << "has_sse:"
    << cpu.has_sse();

  LOG(INFO)
    << "has_avx:"
    << cpu.has_avx();
}

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
      ::base::MessageLoop::current()->task_runner()}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  displayMachineInfo();

  int confMinCores
    = kDefaultMinCores;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = pluginInterface->metadata()->configuration();

  if(configuration.hasValue(kConfMinCoresBeforeWarn))
  {
    ::base::StringToInt(
      configuration.value(kConfMinCoresBeforeWarn)
      , &confMinCores);
  }

  checkNumOfCores(
    /// \note Crash if out of range.
    ::base::checked_cast<size_t>(confMinCores));
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

  return VoidPromise::CreateResolved(FROM_HERE);
}

} // namespace extra_logging
} // namespace plugin
