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

#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

namespace plugin {
namespace network_entity {

// UNIQUE type to store in sequence-local-context
STRONGLY_TYPED(basis::PeriodicTaskExecutor, NetworkEntityPeriodicTaskExecutor);

static void setNetworkEntityPeriodicTaskExecutorOnSequence(
  const ::base::Location& from_here
  , scoped_refptr<::base::SequencedTaskRunner> task_runner
  , COPIED() ::base::RepeatingClosure updateCallback)
{
  LOG_CALL(DVLOG(99));

  DCHECK(task_runner
    && task_runner->RunsTasksInCurrentSequence());

  ::base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        from_here, task_runner);

  DCHECK(sequenceLocalContext);
  // Can not register same data type twice.
  // Forces users to call `sequenceLocalContext->unset`.
  DCHECK(!sequenceLocalContext->try_ctx<NetworkEntityPeriodicTaskExecutor>(FROM_HERE));
  ignore_result(sequenceLocalContext->set_once<NetworkEntityPeriodicTaskExecutor>(
        from_here
        , "Timeout.NetworkEntityPeriodicTaskExecutor." + from_here.ToString()
        , std::move(updateCallback)
      ));
}

static void startNetworkEntityPeriodicTaskExecutorOnSequence(
  const ::base::TimeDelta& endTimeDelta)
{
  LOG_CALL(DVLOG(99));

  ::base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, ::base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<NetworkEntityPeriodicTaskExecutor>(FROM_HERE));
  NetworkEntityPeriodicTaskExecutor& executor
    = sequenceLocalContext->ctx<NetworkEntityPeriodicTaskExecutor>(FROM_HERE);

  executor->startPeriodicTimer(
    endTimeDelta);
}

static void unsetNetworkEntityPeriodicTaskExecutorOnSequence()
{
  LOG_CALL(DVLOG(99));

  ::base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, ::base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<NetworkEntityPeriodicTaskExecutor>(FROM_HERE));
  sequenceLocalContext->unset<NetworkEntityPeriodicTaskExecutor>(FROM_HERE);
}

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
  , periodicAsioTaskRunner_(
      ::base::ThreadPool::GetInstance()->
        CreateSequencedTaskRunnerWithTraits(
          ::base::TaskTraits{
            ::base::TaskPriority::BEST_EFFORT
            , ::base::MayBlock()
            , ::base::TaskShutdownBehavior::BLOCK_SHUTDOWN
          }
        ))
  , ioc_{
      mainLoopRegistry_->registry()
        .ctx<::boost::asio::io_context>()}
  , registry_{
      mainLoopRegistry_->registry()
        .ctx<ECS::SafeRegistry>()}
  , networkEntityUpdater_{
      periodicAsioTaskRunner_
      , REFERENCED(registry_)
      , REFERENCED(ioc_)}
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

  return ::base::PostPromise(
    FROM_HERE
    , periodicAsioTaskRunner_.get()
    , ::base::BindOnce(
        &setNetworkEntityPeriodicTaskExecutorOnSequence
        , FROM_HERE
        , periodicAsioTaskRunner_
        , ::base::bindCheckedRepeating(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(&networkEntityUpdater_)
            )
            , &::backend::NetworkEntityUpdater::update
            , ::base::Unretained(&networkEntityUpdater_)
          )
      )
  )
  .ThenOn(periodicAsioTaskRunner_
    , FROM_HERE
    , ::base::BindOnce(
        &startNetworkEntityPeriodicTaskExecutorOnSequence
        , ::base::TimeDelta::FromMilliseconds(
            pluginInterface_->entityUpdateFreqMillisec())
      )
  );
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  return ::base::PostPromise(
    FROM_HERE
    // Post our work to the strand, to prevent data race
    , periodicAsioTaskRunner_.get()
    , ::base::BindOnce(&unsetNetworkEntityPeriodicTaskExecutorOnSequence)
  );
}

} // namespace network_entity
} // namespace plugin
