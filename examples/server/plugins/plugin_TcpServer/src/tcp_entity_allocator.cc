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

#include <basis/scoped_checks.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/ECS/sequence_local_context.hpp>

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

namespace backend {

// see `createOrReuseCached(...)`
struct CreateOrReuseCachedResult
{
  // true if re-used existing entity WITH desired component
  bool usedCache{
    false
  };

  ECS::Entity entity_id{
    ECS::NULL_ENTITY
  };
};

// Will create new entity WITHOUT desired component
// or re-use existing one WITH desired component.
/// @param `ComponentType`
/// Component to create or re-use.
/// @param `UnusedCacheTag`
/// Assume that entity can be re-used from cache (memory pool)
/// only if it was marked with `UnusedCacheTag` (i.e. marked as unused).
/// @param `ExcludeArg`
/// `ExcludeArg` can be used to skip entity with specific tags.
/// For example, entity marked as `NeedToDestroyTag`
/// (i.e. currently deallocating entity) may be ignored.
template<
  typename ComponentType
  , typename UnusedCacheTag
  , class... ExcludeArgs
>
static CreateOrReuseCachedResult createOrReuseCached(
  ECS::Registry& registry)
{
  CreateOrReuseCachedResult result{};

  // Avoid extra allocations
  // with memory pool in ECS style using |UnusedCacheTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    /// \todo use `entt::group` instead of `entt::view` here
    /// if it will speed things up
    /// i.e. measure perf. and compare results
    = registry.view<ComponentType, UnusedCacheTag>(
        entt::exclude<
          ExcludeArgs...
        >
      );

  if(registry_group.empty())
  {
    result.entity_id = registry.create();
    DCHECK(!result.usedCache);
    DCHECK(registry.valid(result.entity_id));
  } else { // if from `cache`
    // try to reuse unused entity (if valid)
    for(const ECS::Entity& entity_id : registry_group)
    {
      if(!registry.valid(entity_id)) {
        // skip invalid entities
        continue;
      }
      result.entity_id = entity_id;
      // need only first valid entity
      break;
    }

    /// \note may not find valid entities,
    /// so check for `ECS::NULL_ENTITY` using `registry.valid`
    if(registry.valid(result.entity_id))
    {
      result.usedCache = true;
      registry.remove<UnusedCacheTag>(result.entity_id);
    } else {
      result.entity_id = registry.create();
      DCHECK(!result.usedCache);
    }
  } // else

  return result;
}

TcpEntityAllocator::TcpEntityAllocator(
  ECS::NetworkRegistry& netRegistry)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , netRegistryRef_(REFERENCED(netRegistry))
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

  DCHECK_MEMBER_OF_UNKNOWN_THREAD(netRegistryRef_);

  DCHECK_RUN_ON_NET_REGISTRY(&(*netRegistryRef_));

  bool usedCache = false;

  /// \note Also processes entities
  /// that were not fully created (see `ECS::DelayedConstruction`).
  /// \note Make sure that not fully created entities are properly freed
  /// (usually that means that they must have some relationship component
  /// like `FirstChildComponent`, `ChildLinkedList` etc.
  /// that will allow them to be freed upon parent entity destruction).
  CreateOrReuseCachedResult allocateResult
    = createOrReuseCached<
        // components to create or re-use
        ECS::TcpConnection
        // components that are not ignored by filter
        , ECS::UnusedTag // only unused entity can be re-used
        // components that must be ignored by filter
        , ECS::NeedToDestroyTag // ignore entity in destruction
      >(
        (*netRegistryRef_).registryUnsafe()
      );

  DCHECK((*netRegistryRef_)->valid(allocateResult.entity_id));

  /// \note `tcpComponent.context.empty()` may be false
  /// if unused `tcpComponent` found using `registry.get`
  /// i.e. we do not fully reset `tcpComponent`,
  /// so reset each type stored in context individually.
  ECS::TcpConnection& tcpComponent
    = (*netRegistryRef_)->get_or_emplace<ECS::TcpConnection>(
        allocateResult.entity_id
        , "TcpConnection_" + base::GenerateGUID() // debug name
      );

  /// We want to add custom components to entity from plugins.
  /// So upon construction, entity must have `ECS::DelayedConstruction` component.
  /// We assume that `entity` will be constructed within 1 tick,
  /// then delete `ECS::DelayedConstruction` component
  /// \note Do not forget to skip entity updates
  /// if it has `ECS::DelayedConstruction` component.
  /// \note Do not forget to properly free entity during termination
  /// if it has `ECS::DelayedConstruction` component
  /// (i.e. app closes while some entity still not constructed).
  /// \note Make sure that not fully created entities are properly freed
  /// (usually that means that they must have some relationship component
  /// like `FirstChildComponent`, `ChildLinkedList` etc.
  /// that will allow them to be freed upon parent entity destruction).
  {
    // mark entity as not fully created
    (*netRegistryRef_)->emplace_or_replace<
        ECS::DelayedConstruction
      >(allocateResult.entity_id);

    // mark entity as not fully created
    (*netRegistryRef_)->remove_if_exists<
        ECS::DelayedConstructionJustDone
      >(allocateResult.entity_id);
  }

  if(usedCache)
  {
    DVLOG(99)
      << "using preallocated network entity with id: "
      << (*netRegistryRef_)->get<ECS::TcpConnection>(allocateResult.entity_id).debug_id;

    /// \todo move `cached entity usage validator` to separate callback
    // Clear cached data.
    // We re-use some old network entity,
    // so need to reset entity state.
    DCHECK((*netRegistryRef_)->valid(allocateResult.entity_id));

    // sanity check
    CHECK((*netRegistryRef_)->has<
        ECS::TcpConnection
      >(allocateResult.entity_id));

    // sanity check
    CHECK(!(*netRegistryRef_)->has<
        ECS::UnusedTag
      >(allocateResult.entity_id));

    // sanity check
    CHECK(!(*netRegistryRef_)->has<
        ECS::NeedToDestroyTag
      >(allocateResult.entity_id));

    // sanity check
    if((*netRegistryRef_)->has<
         base::Optional<DetectChannel::SSLDetectResult>
        >(allocateResult.entity_id))
    {
      // process result once (i.e. do not handle outdated result)
      CHECK((*netRegistryRef_)->has<
          ECS::UnusedSSLDetectResultTag
        >(allocateResult.entity_id));
    }

    // sanity check
    if((*netRegistryRef_)->has<
         base::Optional<Listener::AcceptConnectionResult>
        >(allocateResult.entity_id))
    {
      // process result once (i.e. do not handle outdated result)
      CHECK((*netRegistryRef_)->has<
          ECS::UnusedAcceptResultTag
        >(allocateResult.entity_id));
    }

#if DCHECK_IS_ON()
    /// \note place here only types that can be re-used
    /// i.e. components that can be re-used from cache
    static std::vector<
      DebugComponentIdWithName
    > allowedComponents{
      {
        "TcpConnection"
        , entt::type_info<ECS::TcpConnection>::id()
      }
      , {
        "SSLDetectResult"
        , entt::type_info<base::Optional<DetectChannel::SSLDetectResult>>::id()
      }
      , {
        "UnusedSSLDetectResultTag"
        , entt::type_info<ECS::UnusedSSLDetectResultTag>::id()
      }
      , {
        "AcceptConnectionResult"
        , entt::type_info<base::Optional<Listener::AcceptConnectionResult>>::id()
      }
      , {
        "UnusedAcceptResultTag"
        , entt::type_info<ECS::UnusedAcceptResultTag>::id()
      }
    };

    // visit entity and get the types of components it manages
    (*netRegistryRef_)->visit(allocateResult.entity_id
      , [this, &allocateResult]
      (const entt_type_info_id_type type_id)
      {
        const auto it
          = std::find_if(allowedComponents.begin()
            , allowedComponents.end()
            // custom comparator
            , [&type_id]
            (const DebugComponentIdWithName& x)
            {
              DVLOG(99)
                << " x.id = "
                << x.id
                << " type_id ="
                << type_id;
              /// \note compare only by id, without debug name
              return x.id == type_id;
            }
          );

        if(it == allowedComponents.end())
        {
          DLOG(ERROR)
            << " cached entity with id = "
            << allocateResult.entity_id
            << " is NOT allowed to contain component with id = "
            << type_id;
          NOTREACHED();
        } else {
          DVLOG(99)
            << " cached entity with id = "
            << allocateResult.entity_id
            << " re-uses component with name ="
            << it->debugName
            << " component with id = "
            << type_id;
        }
      }
    );

    ECS::UnsafeTypeContext& ctx
      = tcpComponent.context;

    std::vector<ECS::UnsafeTypeContext::variable_data>& ctxVars
      = ctx.vars();

    /// \note place here only types that can be re-used
    /// i.e. context vars that can be re-used from cache
    static std::vector<
      DebugComponentIdWithName
    > allowedCtxVars{
      {
        "StrandComponent"
        , entt::type_info<Listener::StrandComponent>::id()
      }
      , {
        "HttpChannel"
        , entt::type_info<base::Optional<::flexnet::http::HttpChannel>>::id()
      }
      , {
        "WsChannel"
        , entt::type_info<base::Optional<::flexnet::ws::WsChannel>>::id()
      }
      , {
        "DetectChannel"
        , entt::type_info<base::Optional<::flexnet::http::DetectChannel>>::id()
      }
    };

    for(auto pos = ctxVars.size(); pos; --pos)
    {
      auto type_id = ctxVars[pos-1].type_id;

      const auto it
        = std::find_if(allowedCtxVars.begin()
          , allowedCtxVars.end()
          // custom comparator
          , [&type_id]
          (const DebugComponentIdWithName& x)
          {
            DVLOG(99)
              << " x.id = "
              << x.id
              << " type_id ="
              << type_id;
            /// \note compare only by id, without debug name
            return x.id == type_id;
          }
        );

      if(it == allowedCtxVars.end())
      {
        DLOG(ERROR)
          << " cached context var with id = "
          << allocateResult.entity_id
          << " is NOT allowed to contain context var with id = "
          << type_id;
        NOTREACHED();
      } else {
        DVLOG(99)
          << " cached entity with id = "
          << allocateResult.entity_id
          << " re-uses context var with name ="
          << it->debugName
          << " context var with id = "
          << type_id;
      }
    }
#endif // DCHECK_IS_ON()
  } else {
    DVLOG(99)
      << "allocating new network entity";
  }

  return allocateResult.entity_id;
}

} // namespace backend
