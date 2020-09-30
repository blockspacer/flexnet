#include "ECS/systems/ssl_detect_result.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/util/close_socket_unsafe.hpp>

#include <base/logging.h>
#include <base/guid.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

static constexpr size_t kShutdownExpireTimeoutSec = 5;

void handleSSLDetectResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::http::DetectChannel::SSLDetectResult& detectResult)
{
  using namespace ::flexnet::http;

  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  LOG_CALL(DVLOG(99));

  DCHECK(asio_registry->valid(entity_id));

  /// \note Take care of thread-safety.
  /// We assume that is is safe to change unused asio `stream`
  /// on any thread.
  auto doCloseStream
    = [&detectResult, &asio_registry, entity_id]()
  {
    /// \note we are closing unused stream, so it must be thread-safe here
    if(detectResult.stream.value().socket().is_open()) {
      // Set the timeout.
      ::boost::beast::get_lowest_layer(detectResult.stream.value())
          .expires_after(std::chrono::seconds(
            kShutdownExpireTimeoutSec));
    }

    detectResult.stream.value().close();
  };

  auto doMarkUnused
    = [&detectResult, &asio_registry, entity_id]()
  {
    DCHECK(asio_registry.running_in_this_thread());

    util::closeSocketUnsafe(
      REFERENCED(detectResult.stream.value().socket()));

    DCHECK(asio_registry->valid(entity_id));

    if(!asio_registry->has<ECS::UnusedTag>(entity_id)) {
      asio_registry->emplace<ECS::UnusedTag>(entity_id);
    }
  };

  if(detectResult.need_close)
  {
    LOG(ERROR)
      << "Detector forced shutdown of tcp connection";

    doCloseStream();
    doMarkUnused();

    return;
  }

  // Handle the error, if any
  if (detectResult.ec)
  {
    LOG(ERROR)
      << "Handshake failed for new connection with error: "
      << detectResult.ec.message();

    doCloseStream();
    doMarkUnused();

    return;
  }

  if(detectResult.handshakeResult) {
    LOG(INFO)
      << "Completed secure handshake of new connection";
  } else {
    LOG(INFO)
      << "Completed NOT secure handshake of new connection";
  }

  ECS::TcpConnection& tcpComponent
    = asio_registry->get<ECS::TcpConnection>(entity_id);

  DVLOG(99)
    << "using TcpConnection with id: "
    << tcpComponent.debug_id;

  // Create the http channel and run it
  {
    /// \note it is not ordinary ECS component,
    /// it is stored in entity context (not in ECS registry)
    using HttpChannelCtxComponent
      = base::Optional<::flexnet::http::HttpChannel>;

    HttpChannelCtxComponent* channelCtx
      = &tcpComponent->reset_or_create_var<HttpChannelCtxComponent>(
          "Ctx_http_Channel_" + base::GenerateGUID() // debug name
          , base::rvalue_cast(detectResult.stream.value())
          , base::rvalue_cast(detectResult.buffer)
          , REFERENCED(asio_registry)
          , entity_id);

    // Check that if the value already existed
    // it was overwritten
    {
      DCHECK(channelCtx->value().entityId() == entity_id);
    }

    // start http session
    channelCtx->value().doReadAsync();
  }
}

void updateSSLDetection(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::http;

  using view_component
    = base::Optional<DetectChannel::SSLDetectResult>;

  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  auto registry_group
    = asio_registry->view<view_component>(
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
      [&asio_registry]
      (const auto& entity
       , const auto& component)
    {
      DCHECK(asio_registry->valid(entity));

      handleSSLDetectResult(
        asio_registry
        , entity
        , asio_registry->get<view_component>(entity).value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!asio_registry->has<ECS::UnusedSSLDetectResultTag>(entity)) {
        asio_registry->emplace<ECS::UnusedSSLDetectResultTag>(entity);
      }
    });
}

} // namespace ECS
