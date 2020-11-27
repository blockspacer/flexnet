#include "ECS/systems/unused.hpp" // IWYU pragma: associated

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
#include <base/feature_list.h>
#include <base/trace_event/trace_event.h>

namespace {

constexpr char kFeatureDisableUnusedSystemForTestingName[]
  = "disable_unused_system_for_testing";

// --enable-features=disable_unused_system_for_testing,...
const base::Feature kFeatureDisableUnusedSystemForTesting {
  kFeatureDisableUnusedSystemForTestingName, base::FEATURE_DISABLED_BY_DEFAULT
};

} // namespace

namespace ECS {

void updateUnusedSystem(
  ECS::SafeRegistry& registry)
{
  DCHECK_RUN_ON_REGISTRY(&registry);

  if(UNLIKELY(base::FeatureList::IsEnabled(kFeatureDisableUnusedSystemForTesting)))
  {
    return;
  }

  /// \note Also processes entities
  /// that were not fully created (see `ECS::DelayedConstruction`).
  /// \note Make sure that not fully created entities are properly freed
  /// (usually that means that they must have some relationship component
  /// like `FirstChildComponent`, `ChildSiblings` etc.
  /// that will allow them to be freed upon parent entity destruction).
  auto registry_group
    = registry->view<ECS::UnusedTag>(
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
      DCHECK(!registry->has<ECS::NeedToDestroyTag>(entity_id));
      registry->emplace<ECS::NeedToDestroyTag>(entity_id);
    });
}

} // namespace ECS
