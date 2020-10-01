#include "ECS/systems/unused.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

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

namespace ECS {

void updateUnusedSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  auto registry_group
    = asio_registry->view<ECS::UnusedTag>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >);

  if(registry_group.size()) {
    UMA_HISTOGRAM_COUNTS_1000("ECS.unusedEntitiesBatches",
      // How many entities became unused in single pass.
      registry_group.size());
  }

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " found unused entities, total number: "
      << registry_group.size();
  }
#endif // NDEBUG

  registry_group
    .each(
      [&asio_registry]
      (const auto& entity_id)
    {
      DCHECK(asio_registry->valid(entity_id));
      DCHECK(!asio_registry->has<ECS::NeedToDestroyTag>(entity_id));
      asio_registry->emplace<ECS::NeedToDestroyTag>(entity_id);
    });
}

} // namespace ECS
