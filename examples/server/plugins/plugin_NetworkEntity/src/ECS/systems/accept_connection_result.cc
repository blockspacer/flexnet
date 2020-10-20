#include "ECS/systems/accept_connection_result.hpp" // IWYU pragma: associated

#include <basis/ECS/tags.hpp>

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
  ECS::NetworkRegistry& net_registry
  , const ECS::Entity& entity_id
  , Listener::AcceptConnectionResult& acceptResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK_RUN_ON_NET_REGISTRY(&net_registry);

  DCHECK(net_registry->valid(entity_id));

  numOfHandleAcceptResult++;
  UMA_HISTOGRAM_COUNTS_1000("ECS.handleAcceptResult",
    numOfHandleAcceptResult);

  // each entity representing tcp connection
  // must have that component
  ECS::TcpConnection& tcpComponent
    = net_registry->get<ECS::TcpConnection>(entity_id);

  LOG_CALL(DVLOG(99))
    << " for TcpConnection with id: "
    << tcpComponent.debug_id;

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  auto closeAndReleaseResources
    = [&acceptResult, &net_registry, entity_id]()
  {
    DCHECK(net_registry.RunsTasksInCurrentSequence());

    util::closeSocketUnsafe(
      REFERENCED(acceptResult.socket));

    DCHECK(net_registry->valid(entity_id));

    if(!net_registry->has<ECS::UnusedTag>(entity_id)) {
      net_registry->emplace<ECS::UnusedTag>(entity_id);
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
    = base::Optional<::flexnet::http::DetectChannel>;

  DetectChannelCtxComponent* detectChannelCtx
    = &tcpComponent->reset_or_create_var<DetectChannelCtxComponent>(
        "Ctx_DetectChannel_" + base::GenerateGUID() // debug name
        , base::rvalue_cast(acceptResult.socket)
        , REFERENCED(net_registry)
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
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        base::rvalue_cast(task).Run();
      }
      , base::BindOnce(
        &::flexnet::http::DetectChannel::runDetector
        , base::Unretained(&detectChannelCtx->value())
        // expire timeout for SSL detection
        , std::chrono::seconds(3)
      )
    )
  );
}

void updateNewConnections(
  ECS::NetworkRegistry& net_registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = base::Optional<Listener::AcceptConnectionResult>;

  DCHECK_RUN_ON_NET_REGISTRY(&net_registry);

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    = net_registry->view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity is unused
          , ECS::UnusedTag
          // components related to acceptor are unused
          , ECS::UnusedAcceptResultTag
        >
      );

  registry_group
    .each(
      [&net_registry]
      (const ECS::Entity& entity
       , const auto& component)
    {
      DCHECK(net_registry->valid(entity));

      handleAcceptResult(
        net_registry
        , entity
        , net_registry->get<view_component>(entity).value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!net_registry->has<ECS::UnusedAcceptResultTag>(entity)) {
        net_registry->emplace<ECS::UnusedAcceptResultTag>(entity);
      }
    });
}

} // namespace ECS
