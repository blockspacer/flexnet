#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

#include <base/numerics/safe_conversions.h>

#include <basis/log/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/status/statusor.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/strong_types/strong_alias.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <iostream>

#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

namespace plugin {
namespace network_registry {

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
  , registry_{
      mainLoopRegistry_->registry()
        .set<ECS::SafeRegistry>()}
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

  return VoidPromise::CreateResolved(FROM_HERE);
}

} // namespace network_registry
} // namespace plugin
