#include "ECS/systems/delayed_construction_just_done.hpp" // IWYU pragma: associated

#include <basis/ECS/tags.hpp>

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

void updateDelayedConstructionJustDone(
  ECS::SafeRegistry& registry)
{
  DCHECK_RUN_ON_REGISTRY(&registry);

  auto registry_group
    = registry->view<ECS::DelayedConstructionJustDone>(
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
      [&registry]
      (const ECS::Entity& entity_id)
    {
      DCHECK(registry->valid(entity_id));
      DCHECK(!registry->has<ECS::DelayedConstruction>(entity_id));
      DCHECK(registry->has<ECS::DelayedConstructionJustDone>(entity_id));
      registry->remove<ECS::DelayedConstructionJustDone>(entity_id);
    });
}

} // namespace ECS
