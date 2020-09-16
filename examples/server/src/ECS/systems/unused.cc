#include "ECS/systems/unused.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

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
      (const auto& entity)
    {
      DCHECK(asio_registry->valid(entity)
        && !asio_registry->has<ECS::NeedToDestroyTag>(entity));
      asio_registry->emplace<ECS::NeedToDestroyTag>(entity);
    });
}

} // namespace ECS
