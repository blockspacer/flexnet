#include "tcp_entity_allocator.hpp" // IWYU pragma: associated

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/tags.hpp>
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

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/asio_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/move_only.hpp>
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

TcpEntityAllocator::TcpEntityAllocator(
  ECS::AsioRegistry& asioRegistry)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , asioRegistry_(REFERENCED(asioRegistry))
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

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(asioRegistry_));

  DCHECK_RUN_ON_STRAND(&asioRegistry_.strand, ECS::AsioRegistry::ExecutorType);

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    /// \todo use `entt::group` instead of `entt::view` here
    /// if it will speed things up
    /// i.e. measure perf. and compare results
    = asioRegistry_->view<ECS::TcpConnection, ECS::UnusedTag>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >
      );

  ECS::Entity tcp_entity_id
    = ECS::NULL_ENTITY;

  bool usedCache = false;

  if(registry_group.empty())
  {
    tcp_entity_id = asioRegistry_->create();
    DCHECK(!usedCache);
    DVLOG(99)
      << "allocating new network entity";
    DCHECK(asioRegistry_->valid(tcp_entity_id));
  } else { // if from `cache`
    // reuse any unused entity (if found)
    for(const ECS::Entity& entity_id : registry_group)
    {
      if(!asioRegistry_->valid(entity_id)) {
        // skip invalid entities
        continue;
      }
      tcp_entity_id = entity_id;
      // need only first valid entity
      break;
    }

    /// \note may not find valid entities,
    /// so checks for `ECS::NULL_ENTITY` using `registry.valid`
    if(asioRegistry_->valid(tcp_entity_id))
    {
      usedCache = true;
      asioRegistry_->remove<ECS::UnusedTag>(tcp_entity_id);
      DVLOG(99)
        << "using preallocated network entity with id: "
        << asioRegistry_->get<ECS::TcpConnection>(tcp_entity_id).debug_id;

    } else {
      tcp_entity_id = asioRegistry_->create();
      DCHECK(!usedCache);
      DVLOG(99)
        << "allocating new network entity";
    }
  } // else

  DCHECK(asioRegistry_->valid(tcp_entity_id));

  /// \note `tcpComponent.context.empty()` may be false
  /// if unused `tcpComponent` found using `registry.get`
  /// i.e. we do not fully reset `tcpComponent`,
  /// so reset each type stored in context individually.
  ECS::TcpConnection& tcpComponent
    = asioRegistry_->get_or_emplace<ECS::TcpConnection>(
        tcp_entity_id
        , "TcpConnection_" + base::GenerateGUID() // debug name
      );

  if(usedCache)
  {
    /// \todo move `cached entity usage validator` to separate callback
    // Clear cached data.
    // We re-use some old network entity,
    // so need to reset entity state.
    DCHECK(asioRegistry_->valid(tcp_entity_id));

    // sanity check
    CHECK(asioRegistry_->has<
        ECS::TcpConnection
      >(tcp_entity_id));

    // sanity check
    CHECK(!asioRegistry_->has<
        ECS::UnusedTag
      >(tcp_entity_id));

    // sanity check
    CHECK(!asioRegistry_->has<
        ECS::NeedToDestroyTag
      >(tcp_entity_id));

    // required to process `SSLDetectResult` once
    // (i.e. not handle outdated result)
    asioRegistry_->emplace_or_replace<
      ECS::UnusedSSLDetectResultTag
    >(tcp_entity_id);

    // required to process `AcceptResult` once
    // (i.e. not handle outdated result)
    asioRegistry_->emplace_or_replace<
      ECS::UnusedAcceptResultTag
    >(tcp_entity_id);

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
    asioRegistry_->visit(tcp_entity_id
      , [this, tcp_entity_id]
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
            << tcp_entity_id
            << " is NOT allowed to contain component with id = "
            << type_id;
          NOTREACHED();
        } else {
          DVLOG(99)
            << " cached entity with id = "
            << tcp_entity_id
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
          << tcp_entity_id
          << " is NOT allowed to contain context var with id = "
          << type_id;
        NOTREACHED();
      } else {
        DVLOG(99)
          << " cached entity with id = "
          << tcp_entity_id
          << " re-uses context var with name ="
          << it->debugName
          << " context var with id = "
          << type_id;
      }
    }
#endif // DCHECK_IS_ON()
  } // if

  return tcp_entity_id;
}

} // namespace backend
