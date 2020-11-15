#include "network_entity_updater.hpp" // IWYU pragma: associated

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/unused_child_list.hpp"
#include "ECS/systems/delayed_construction.hpp"
#include "ECS/systems/delayed_construction_just_done.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/guid.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/feature_list.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <base/trace_event/memory_dump_manager.h>
#include <base/trace_event/heap_profiler.h>
#include <base/trace_event/heap_profiler_allocation_context_tracker.h>
#include <base/trace_event/heap_profiler_event_filter.h>
#include <base/sampling_heap_profiler/sampling_heap_profiler.h>
#include <base/sampling_heap_profiler/poisson_allocation_sampler.h>
#include <base/sampling_heap_profiler/module_cache.h>
#include <base/profiler/frame.h>
#include <base/trace_event/malloc_dump_provider.h>
#include <base/trace_event/memory_dump_provider.h>
#include <base/trace_event/memory_dump_scheduler.h>
#include <base/trace_event/memory_infra_background_whitelist.h>
#include <base/trace_event/process_memory_dump.h>
#include <base/trace_event/trace_event.h>

#include <basis/scoped_checks.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

NetworkEntityUpdater::NetworkEntityUpdater(
  scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner
  , ECS::NetworkRegistry& netRegistry
  , boost::asio::io_context& ioc)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , netRegistry_(REFERENCED(netRegistry))
  , ioc_(REFERENCED(ioc))
  , periodicAsioTaskRunner_(periodicAsioTaskRunner)
  , periodicTaskExecutor_(
      base::BindRepeating(
        &NetworkEntityUpdater::update
        /// \note callback may be skipped if `WeakPtr` becomes invalid
        , weakSelf()
      )
    )
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NetworkEntityUpdater::~NetworkEntityUpdater()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void NetworkEntityUpdater::update() NO_EXCEPTION
{
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(netRegistry_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(periodicAsioTaskRunner_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(ioc_);

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicAsioTaskRunner_.get());

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  if(ioc_->stopped()) // io_context::stopped is thread-safe
  {
    LOG(WARNING)
      << "skipping update of registry"
         " because of stopped io context";

    // make sure that all allocated
    // `per-connection resources` are freed
    // i.e. use check `registry.empty()`
    {
      /// \note (thread-safety) access when ioc->stopped
      /// i.e. assume no running asio threads that use |netRegistry_|
      DCHECK(netRegistry_->registryUnsafe().empty());
    }

    return;
  }

  netRegistry_->taskRunner()->PostTask(
    FROM_HERE
    , base::BindOnce([
      ](
        ECS::NetworkRegistry& net_registry
      ){
        DCHECK(
          net_registry.RunsTasksInCurrentSequence());

        ECS::updateNewConnections(net_registry);

        ECS::updateSSLDetection(net_registry);

        /// \todo customizable cleanup period
        ECS::updateUnusedChildList<
          // drop recieved messages
          // if websocket session is unused i.e. closed
          ::flexnet::ws::WsChannel::RecievedData
        >(net_registry);

        /// \note Removes `DelayedConstructionJustDone` component from any entity.
        ECS::updateDelayedConstructionJustDone(net_registry);

        /// \note Removes `DelayedConstruction` component from any entity
        /// and adds `DelayedConstructionJustDone` component.
        ECS::updateDelayedConstruction(net_registry);

        /// \todo customizable cleanup period
        ECS::updateUnusedSystem(net_registry);

        /// \todo customizable cleanup period
        ECS::updateCleanupSystem(net_registry);
      }
      , REFERENCED(*netRegistry_)
    )
  );
}

MUST_USE_RETURN_VALUE
basis::PeriodicTaskExecutor&
  NetworkEntityUpdater::periodicTaskExecutor() NO_EXCEPTION
{
  DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(periodicTaskExecutor);

  DCHECK_MEMBER_OF_UNKNOWN_THREAD(periodicTaskExecutor_);
  return periodicTaskExecutor_;
}

} // namespace backend
