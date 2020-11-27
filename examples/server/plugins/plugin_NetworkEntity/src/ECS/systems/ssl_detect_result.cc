#include "ECS/systems/ssl_detect_result.hpp" // IWYU pragma: associated

#include <basis/ECS/tags.hpp>
#include <basis/ECS/helpers/lifetime/exclude_not_constructed.hpp>

#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/http/http_channel.hpp>
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
#include <base/guid.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

static constexpr size_t kShutdownExpireTimeoutSec = 5;

static size_t numOfHandleSSLDetectResult = 0;

void handleSSLDetectResult(
  ECS::SafeRegistry& registry
  , const ECS::Entity& entity_id
  , flexnet::http::DetectChannel::SSLDetectResult& detectResult)
{
  using namespace ::flexnet::http;

  DCHECK_RUN_ON_REGISTRY(&registry);

  LOG_CALL(DVLOG(99));

  DCHECK(registry->valid(entity_id));

  numOfHandleSSLDetectResult++;
  UMA_HISTOGRAM_COUNTS_1000("ECS.handleSSLDetectResult",
    numOfHandleSSLDetectResult);

  /// \note Take care of thread-safety.
  /// We assume that is is safe to change unused asio `stream`
  /// on any thread.
  auto doCloseStream
    = [&detectResult, &registry, entity_id]()
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
    = [&detectResult, &registry, entity_id]()
  {
    DCHECK_RUN_ON_REGISTRY(&registry);

    util::closeSocketUnsafe(
      REFERENCED(detectResult.stream.value().socket()));

    DCHECK(registry->valid(entity_id));

    if(!registry->has<ECS::UnusedTag>(entity_id)) {
      registry->emplace<ECS::UnusedTag>(entity_id);
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
    = registry->get<ECS::TcpConnection>(entity_id);

  DVLOG(99)
    << "using TcpConnection with id: "
    << tcpComponent.debug_id;

  // Create the http channel and run it
  {
    /// \note it is not ordinary ECS component,
    /// it is stored in entity context (not in ECS registry)
    using HttpChannelCtxComponent
      = ::base::Optional<::flexnet::http::HttpChannel>;

    HttpChannelCtxComponent* channelCtx
      = &tcpComponent->reset_or_create_var<HttpChannelCtxComponent>(
          "Ctx_http_Channel_" + ::base::GenerateGUID() // debug name
          , ::base::rvalue_cast(detectResult.stream.value())
          , ::base::rvalue_cast(detectResult.buffer)
          , REFERENCED(registry)
          , entity_id);

    // Check that if the value already existed
    // it was overwritten
    {
      DCHECK(channelCtx->value().entityId() == entity_id);
    }

    // start http session
    channelCtx->value().startReadAsync();
  }
}

void updateSSLDetection(
  ECS::SafeRegistry& registry)
{
  using namespace ::flexnet::http;

  using view_component
    = ::base::Optional<DetectChannel::SSLDetectResult>;

  DCHECK_RUN_ON_REGISTRY(&registry);

  auto registry_group
    = registry->view<view_component>(
        ECS::exclude_not_constructed<
          // components related to SSL detection are unused
          ECS::UnusedSSLDetectResultTag
        >
      );

  registry_group
    .each(
      [&registry]
      (const ECS::Entity& entity
       , view_component& component)
    {
      DCHECK(registry->valid(entity));

      handleSSLDetectResult(
        registry
        , entity
        , component.value());

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry->has<ECS::UnusedSSLDetectResultTag>(entity)) {
        registry->emplace<ECS::UnusedSSLDetectResultTag>(entity);
      }
    });
}

} // namespace ECS
