#include "tcp_entity_allocator.hpp" // IWYU pragma: associated

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

#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/ECS/helpers/algorithm/find_entity.hpp>
#include <basis/ECS/helpers/lifetime/populate_associated_components.hpp>
#include <basis/ECS/helpers/lifetime/populate_delayed_construction_components.hpp>
#include <basis/ECS/helpers/lifetime/exclude_not_constructed.hpp>
#include <basis/ECS/helpers/lifetime/reset_on_cache_reuse.hpp>
#include <basis/ECS/helpers/algorithm/check_components_whitest.hpp>
#include <basis/ECS/helpers/algorithm/check_context_vars_whitest.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace {

#if DCHECK_IS_ON()
// Must match type of `entt::type_info<SomeType>::id()`
using entt_type_info_id_type
  = std::uint32_t;

// Used to prohibit unknown types of components.
struct DebugComponentIdWithName {
  std::string debugName;
  entt_type_info_id_type id;
};
#endif // DCHECK_IS_ON()

} // namespace

namespace ECS {

template<>
void populateAssociatedComponents<ECS::TcpConnection>(
  ECS::Registry& registry
  , ECS::Entity entityId)
{
  DCHECK(registry.valid(entityId));

  /// \note `tcpComponent.context.empty()` may be false
  /// if unused `tcpComponent` found using `registry.get`
  /// i.e. we do not fully reset `tcpComponent`,
  /// so reset each type stored in context individually.
  ECS::TcpConnection& tcpComponent
    = registry.template get_or_emplace<ECS::TcpConnection>(
        entityId
        , "TcpConnection_" + ::base::GenerateGUID() // debug name
      );

  ECS::populateDelayedConstructionComponents(registry, entityId);
}

// Clear cached data.
// We re-use some old network entity,
// so need to reset entity state.
template<>
void resetOnCacheReuse<ECS::TcpConnection>(
  ECS::Registry& registry
  , ECS::Entity entityId)
{
  DCHECK(registry.valid(entityId));

  using DetectChannel
    = ::flexnet::http::DetectChannel;

  using Listener
    = ::flexnet::ws::Listener;

  using WsChannel
    = ::flexnet::ws::WsChannel;

  using HttpChannel
    = ::flexnet::http::HttpChannel;

  ECS::populateAssociatedComponents<ECS::TcpConnection>(registry, entityId);

  // make sure all components that entity uses can be cached
  // (reset newly added components via `reset_or_create_component`,
  // than add them here)
  DCHECK_COMPONENT_WHITELIST(
    REFERENCED(registry)
    , entityId
    , ECS::include<
        ECS::TcpConnection
        , ::base::Optional<DetectChannel::SSLDetectResult>
        , ECS::UnusedSSLDetectResultTag
        , ::base::Optional<Listener::AcceptConnectionResult>
        , ECS::UnusedAcceptResultTag
      >);

  ECS::TcpConnection& tcpConnection
    = registry.get<ECS::TcpConnection>(entityId);

  ECS::UnsafeTypeContext& ctx
    = tcpConnection.context;

  // make sure all context vars that entity uses can be cached
  // (reset newly added context vars via `reset_or_create_var`,
  // than add them here)
  DCHECK_CONTEXT_VARS_WHITELIST(
    REFERENCED(ctx)
    , ECS::include<
        ::base::Optional<DetectChannel>
        , ::base::Optional<WsChannel>
        , ::base::Optional<HttpChannel>
        , Listener::StrandComponent
      >);

  if(registry.has<
       ::base::Optional<DetectChannel::SSLDetectResult>
      >(entityId))
  {
    // process result once (i.e. do not handle outdated result)
    CHECK(registry.has<
        ECS::UnusedSSLDetectResultTag
      >(entityId));
  }

  if(registry.has<
       ::base::Optional<Listener::AcceptConnectionResult>
      >(entityId))
  {
    // process result once (i.e. do not handle outdated result)
    CHECK(registry.has<
        ECS::UnusedAcceptResultTag
      >(entityId));
  }
}

} // namespace ECS

namespace backend {

TcpEntityAllocator::TcpEntityAllocator(
  ECS::SafeRegistry& registry)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , registryRef_(REFERENCED(registry))
{
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

TcpEntityAllocator::~TcpEntityAllocator()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

ECS::Entity TcpEntityAllocator::allocateTcpEntity() NO_EXCEPTION
{
  using DetectChannel
    = ::flexnet::http::DetectChannel;

  using Listener
    = ::flexnet::ws::Listener;

  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_OF_UNKNOWN_THREAD(registryRef_);

  DCHECK_RUN_ON_REGISTRY(&(*registryRef_));

  ECS::Registry& registry = *registryRef_;

  // Avoid extra allocations
  // with memory pool in ECS style using |Include...|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  bool usedCache = false;

  /// \note Also processes entities
  /// that were not fully created (see `ECS::DelayedConstruction`).
  /// \note Make sure that not fully created entities are properly freed
  /// (usually that means that they must have some relationship component
  /// like `FirstChildComponent`, `ChildSiblings` etc.
  /// that will allow them to be freed upon parent entity destruction).
  ECS::Entity entityId
    = ECS::findEntity(
        registry
        , ECS::include<
            ECS::TcpConnection
            , ECS::UnusedTag
          >
        , ECS::exclude<
            // ignore entity in destruction
            ECS::NeedToDestroyTag
          >
      );

  if(entityId == ECS::NULL_ENTITY)
  {
    DVLOG(99)
      << "allocating new network entity";
    entityId = registry.create();
    ECS::populateAssociatedComponents<ECS::TcpConnection>(registry, entityId);
  } else {
    DVLOG(99)
      << "using preallocated network entity";
    usedCache = true;
    ECS::resetOnCacheReuse<ECS::TcpConnection>(registry, entityId);
  }

  return entityId;
}

} // namespace backend
