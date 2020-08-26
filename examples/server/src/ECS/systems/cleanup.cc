#include "ECS/systems/cleanup.hpp" // IWYU pragma: associated

#include <base/logging.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

void updateCleanupSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry.ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<ECS::NeedToDestroyTag>();

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " cleaning up entities, total number: "
      << registry_group.size();
  }
  registry_group
    .each(
      [&registry]
      (const ECS::Entity& entity)
    {
      DCHECK(registry.valid(entity));
    });
#endif // NDEBUG

  registry.destroy(
    registry_group.begin()
    , registry_group.end());
}

} // namespace ECS
