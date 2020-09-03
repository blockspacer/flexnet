#include "ECS/systems/cleanup.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

void updateCleanupSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK(asio_registry.running_in_this_thread());

  auto registry_group
    = asio_registry->view<ECS::NeedToDestroyTag>();

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
