#include "ECS/systems/cleanup.hpp" // IWYU pragma: associated

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

static size_t numOfCleanedUpEntities = 0;

void updateCleanupSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  auto registry_group
    = asio_registry->view<ECS::NeedToDestroyTag>();

  if(registry_group.size()) {
    UMA_HISTOGRAM_COUNTS_1000("ECS.cleanupBatches",
      // How many entities will free in single pass.
      registry_group.size());

    numOfCleanedUpEntities += registry_group.size();
    UMA_HISTOGRAM_COUNTS_1000("ECS.numOfCleanedUpEntities",
      numOfCleanedUpEntities);
  }

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " cleaning up entities, total number: "
      << registry_group.size();
  }
  registry_group
    .each(
      [&asio_registry]
      (const ECS::Entity& entity)
    {
      DCHECK(asio_registry->valid(entity));
    });
#endif // NDEBUG

  asio_registry->destroy(
    registry_group.begin()
    , registry_group.end());
}

} // namespace ECS
