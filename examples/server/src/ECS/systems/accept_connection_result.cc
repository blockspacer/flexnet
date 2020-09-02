#include "ECS/systems/accept_connection_result.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

void handleAcceptNewConnectionResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::ws::Listener::AcceptConnectionResult& acceptResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
    .ref_registry(FROM_HERE);

  ECS::AsioRegistry::StrandType asioRegistryStrand
    = asio_registry
    .ref_strand(FROM_HERE);

  DCHECK(asioRegistryStrand.running_in_this_thread());

  // Handle the error, if any
  if (acceptResult.ec)
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << acceptResult.ec.message();

    // it is safe to destroy entity now
    if(!registry.has<ECS::UnusedTag>(entity_id)) {
      registry.emplace<ECS::UnusedTag>(entity_id);
    }

    return;
  }

  LOG(INFO)
    << "Listener accepted new connection";

  using DetectChannelComponent
    = base::Optional<::flexnet::http::DetectChannel>;

  const bool useCache
    = registry.has<DetectChannelComponent>(entity_id);

  DCHECK(asioRegistryStrand.running_in_this_thread());
  DetectChannelComponent& detectChannelRef
    = useCache
      ? registry.get<DetectChannelComponent>(entity_id)
      : registry
      .emplace<DetectChannelComponent>(
        entity_id
        , base::in_place
        , base::rvalue_cast(acceptResult.socket)
        , RAW_REFERENCED(asio_registry)
        , entity_id);
  DCHECK(registry.has<DetectChannelComponent>(entity_id));

  if(useCache) {
    DCHECK(detectChannelRef);
    detectChannelRef.emplace(
      base::rvalue_cast(acceptResult.socket)
      , RAW_REFERENCED(asio_registry)
      , entity_id);
    DVLOG(99)
      << "using preallocated DetectChannel";
  } else {
    DVLOG(99)
      << "allocating new DetectChannel";
  }

  // working executor required by |::boost::asio::post|
  ::boost::asio::post(
    detectChannelRef.value().perConnectionStrand()
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        base::rvalue_cast(task).Run();
      }
      , base::BindOnce(
        &::flexnet::http::DetectChannel::runDetector
        , UNOWNED_LIFETIME(base::Unretained(&detectChannelRef.value()))
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

  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry.
      ref_registry(FROM_HERE);

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
