#include "ECS/systems/accept_connection_result.hpp" // IWYU pragma: associated

#include <basis/ECS/tags.hpp>
#include <basis/task/task_util.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <flexnet/util/close_socket_unsafe.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/util/close_socket_unsafe.hpp>

#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/metrics/histogram_functions.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/guid.h>

namespace ECS {

using Listener
  = flexnet::ws::Listener;

static size_t numOfHandleAcceptResult = 0;

void handleAcceptResult(
  ECS::SafeRegistry& registry
  , const ECS::Entity& entity_id
  , Listener::AcceptConnectionResult& acceptResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK_RUN_ON_REGISTRY(&registry);

  DCHECK(registry->valid(entity_id));

  numOfHandleAcceptResult++;
  UMA_HISTOGRAM_COUNTS_1000("ECS.handleAcceptResult",
    numOfHandleAcceptResult);

  // each entity representing tcp connection
  // must have that component
  ECS::TcpConnection& tcpComponent
    = registry->get<ECS::TcpConnection>(entity_id);

  LOG_CALL(DVLOG(99))
    << " for TcpConnection with id: "
    << tcpComponent.debug_id;

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  auto closeAndReleaseResources
    = [&acceptResult, &registry, entity_id]()
  {
    DCHECK_RUN_ON_REGISTRY(&registry);

    util::closeSocketUnsafe(
      REFERENCED(acceptResult.socket));

    DCHECK(registry->valid(entity_id));

    if(!registry->has<ECS::UnusedTag>(entity_id)) {
      registry->emplace<ECS::UnusedTag>(entity_id);
    }
  };

  // Handle the error, if any
  if (acceptResult.ec)
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << acceptResult.ec.message();

    closeAndReleaseResources();

    return;
  }

  DVLOG(99)
    << "Listener accepted new connection";

  if(acceptResult.need_close)
  {
    DVLOG(99)
      << "forced shutdown of tcp connection";

    closeAndReleaseResources();

    // nothing to do
    return;
  }

  /// \note it is not ordinary ECS component,
  /// it is stored in entity context (not in ECS registry)
  using DetectChannelCtxComponent
    = ::base::Optional<::flexnet::http::DetectChannel>;

  DetectChannelCtxComponent* detectChannelCtx
    = &tcpComponent->reset_or_create_var<DetectChannelCtxComponent>(
        "Ctx_DetectChannel_" + ::base::GenerateGUID() // debug name
        , ::base::rvalue_cast(acceptResult.socket)
        , REFERENCED(registry)
        , entity_id);

  // Check that if the value already existed
  // it was overwritten
  {
    DCHECK(detectChannelCtx->value().entityId() == entity_id);
    DCHECK(detectChannelCtx->value().isDetected() == false);
  }

  // working executor required by |::boost::asio::post|
  ::boost::asio::post(
    detectChannelCtx->value().perConnectionStrand()
    , ::basis::bindFrontOnceClosure(
        ::base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(&detectChannelCtx->value())
          )
          , &::flexnet::http::DetectChannel::runDetector
          , ::base::Unretained(&detectChannelCtx->value())
          // expire timeout for SSL detection
          , std::chrono::seconds(3)))
  );
}

void updateNewConnections(
  ECS::SafeRegistry& registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = ::base::Optional<Listener::AcceptConnectionResult>;

  DCHECK_RUN_ON_REGISTRY(&registry);

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    = registry->view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity not fully created
          , ECS::DelayedConstruction
          // entity is unused
          , ECS::UnusedTag
          // components related to acceptor are unused
          , ECS::UnusedAcceptResultTag
        >
      );

  registry_group
    .each(
      [&registry]
      (const ECS::Entity& entity
       , view_component& component)
    {
      DCHECK(registry->valid(entity));

      handleAcceptResult(
        registry
        , entity
        , component.value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry->has<ECS::UnusedAcceptResultTag>(entity)) {
        registry->emplace<ECS::UnusedAcceptResultTag>(entity);
      }
    });
}

} // namespace ECS
