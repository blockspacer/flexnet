#include "net/network_entity_updater.hpp" // IWYU pragma: associated

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

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

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/asio_registry.hpp>
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

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

NetworkEntityUpdater::NetworkEntityUpdater(
  scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner
  , ECS::AsioRegistry& asioRegistry
  , boost::asio::io_context& ioc)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , asioRegistry_(REFERENCED(asioRegistry))
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
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NetworkEntityUpdater::~NetworkEntityUpdater()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void NetworkEntityUpdater::update() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicAsioTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);

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
      /// i.e. assume no running asio threads that use |asioRegistry_|
      DCHECK_RUN_ON_ANY_THREAD_SCOPE(asioRegistry_->fn_registry);
      DCHECK(asioRegistry_->registry().empty());
    }

    return;
  }

  ::boost::asio::post(
    asioRegistry_->asioStrand()
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
      ](
        ECS::AsioRegistry& asio_registry
      ){
        DCHECK(
          asio_registry.running_in_this_thread());

        ECS::updateNewConnections(asio_registry);

        ECS::updateSSLDetection(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateClosingSockets(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateUnusedSystem(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateCleanupSystem(asio_registry);
      }
      , REFERENCED(*asioRegistry_)
    )
  );
}

MUST_USE_RETURN_VALUE
basis::PeriodicTaskExecutor&
  NetworkEntityUpdater::periodicTaskExecutor() NO_EXCEPTION
{
  DCHECK_RUN_ON_ANY_THREAD_SCOPE(fn_periodicTaskExecutor);

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicTaskExecutor_);
  return periodicTaskExecutor_;
}

} // namespace backend
