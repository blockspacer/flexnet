#include "ECS/systems/ssl_detect_result.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

void handleSSLDetectResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::http::DetectChannel::SSLDetectResult& detectResult)
{
  using namespace ::flexnet::http;

  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
      .ref_registry(FROM_HERE);

  ECS::AsioRegistry::StrandType asioRegistryStrand
    = asio_registry.ref_strand(FROM_HERE);

  DCHECK(asioRegistryStrand.running_in_this_thread());

  LOG_CALL(DVLOG(9));

  auto closeAndReleaseResources
    = [&detectResult, &registry, entity_id]()
  {
    // Send shutdown
    if(detectResult.stream.value().socket().is_open())
    {
      DVLOG(9) << "shutdown of stream...";
      boost::beast::error_code ec;
      detectResult.stream.value().socket().shutdown(
        boost::asio::ip::tcp::socket::shutdown_send, ec);
      if (ec) {
        LOG(WARNING)
          << "error during stream shutdown: "
          << ec.message();
      }
    }

    // it is safe to destroy entity now
    if(!registry.has<ECS::UnusedTag>(entity_id)) {
      registry.emplace<ECS::UnusedTag>(entity_id);
    }
  };

  // Handle the error, if any
  if (detectResult.ec)
  {
    LOG(ERROR)
      << "Handshake failed for new connection with error: "
      << detectResult.ec.message();

    closeAndReleaseResources();

    DCHECK(!detectResult.stream.value().socket().is_open());

    return;
  }

  if(detectResult.handshakeResult) {
    LOG(INFO)
      << "Completed secure handshake of new connection";
  } else {
    LOG(INFO)
      << "Completed NOT secure handshake of new connection";
  }

  DCHECK(detectResult.stream.value().socket().is_open());

  /// \todo: create channel here
  // Create the session and run it
  //std::make_shared<session>(base::rvalue_cast(detectResult.stream.value().socket()))->run();
  closeAndReleaseResources();
}

void updateSSLDetection(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::http;

  using view_component
    = base::Optional<DetectChannel::SSLDetectResult>;

  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
      .ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity is unused
          , ECS::UnusedTag
          // components related to SSL detection are unused
          , ECS::UnusedSSLDetectResultTag
        >
      );

  registry_group
    .each(
      [&registry, &asio_registry]
      (const auto& entity
       , const auto& component)
    {
      DCHECK(registry.valid(entity));

      handleSSLDetectResult(
        asio_registry
        , entity
        , registry.get<view_component>(entity).value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry.has<ECS::UnusedSSLDetectResultTag>(entity)) {
        registry.emplace<ECS::UnusedSSLDetectResultTag>(entity);
      }
    });
}

} // namespace ECS
