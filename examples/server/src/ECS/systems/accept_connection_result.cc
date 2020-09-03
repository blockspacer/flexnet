#include "ECS/systems/accept_connection_result.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/guid.h>

namespace ECS {

void handleAcceptNewConnectionResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::ws::Listener::AcceptConnectionResult& acceptResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK(asio_registry.running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
    .ref_registry(FROM_HERE);

  // each entity representing tcp connection
  // must have that component
  ECS::TcpConnection& tcpComponent
    = registry.get<ECS::TcpConnection>(entity_id);

  LOG_CALL(DVLOG(99))
    << " for TcpConnection with id: "
    << tcpComponent.debug_id;

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<flexnet::ws::Listener::StrandComponent>());

  // Handle the error, if any
  if (acceptResult.ec)
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << acceptResult.ec.message();

    // Schedule shutdown on asio thread
    if(!registry.has<ECS::CloseSocket>(entity_id)) {
      registry.emplace<ECS::CloseSocket>(entity_id
        /// \todo use UnownedPtr
        , UNOWNED_LIFETIME() &acceptResult.socket);
    }

    return;
  }

  DVLOG(99)
    << "Listener accepted new connection";

  if(acceptResult.need_close)
  {
    DVLOG(99)
      << "Listener forced shutdown of created connection";

    // Schedule shutdown on asio thread
    if(!registry.has<ECS::CloseSocket>(entity_id)) {
      registry.emplace<ECS::CloseSocket>(entity_id
        /// \todo use UnownedPtr
        , UNOWNED_LIFETIME() &acceptResult.socket);
    }

    // nothing to do
    return;
  }

  /// \note it is not ordinary ECS component,
  /// it is stored in entity context (not in ECS registry)
  using DetectChannelCtxComponent
    = base::Optional<::flexnet::http::DetectChannel>;

  const bool useCache
    = tcpComponent->try_ctx_var<DetectChannelCtxComponent>();

  DVLOG(99)
    << (useCache
        ? "using preallocated DetectChannel"
        : "allocating new DetectChannel");

  DetectChannelCtxComponent* detectChannelCtx
    = &tcpComponent->ctx_or_set_var<DetectChannelCtxComponent>(
        "Ctx_DetectChannel_" + base::GenerateGUID() // debug name
        , base::in_place
        , base::rvalue_cast(acceptResult.socket)
        , RAW_REFERENCED(asio_registry)
        , entity_id);

  // If the value already exists it is overwritten
  if(useCache) {
    detectChannelCtx = &tcpComponent->set_var<DetectChannelCtxComponent>(
        "Ctx_DetectChannel_" + base::GenerateGUID() // debug name
        , base::in_place
        , base::rvalue_cast(acceptResult.socket)
        , RAW_REFERENCED(asio_registry)
        , entity_id);
  }

  // Check that if the value already existed
  // it was overwritten
  {
    DCHECK(detectChannelCtx->value().entity_id() == entity_id);
    DCHECK(detectChannelCtx->value().isDetected() == false);
  }

  // working executor required by |::boost::asio::post|
  ::boost::asio::post(
    detectChannelCtx->value().perConnectionStrand()
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        base::rvalue_cast(task).Run();
      }
      , base::BindOnce(
        &::flexnet::http::DetectChannel::runDetector
        , UNOWNED_LIFETIME(base::Unretained(&detectChannelCtx->value()))
        // expire timeout for SSL detection
        , std::chrono::seconds(3)
      )
    )
  );
}

void updateNewConnections(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = base::Optional<Listener::AcceptConnectionResult>;

  ECS::Registry& registry
    = asio_registry.
      ref_registry(FROM_HERE);

  DCHECK(asio_registry.running_in_this_thread());

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    = registry.view<view_component>(
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
      [&registry, &asio_registry]
      (const auto& entity
       , const auto& component)
    {
      DCHECK(registry.valid(entity));

      handleAcceptNewConnectionResult(
        asio_registry
        , entity
        , registry.get<view_component>(entity).value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry.has<ECS::UnusedAcceptResultTag>(entity)) {
        registry.emplace<ECS::UnusedAcceptResultTag>(entity);
      }
    });
}

} // namespace ECS
