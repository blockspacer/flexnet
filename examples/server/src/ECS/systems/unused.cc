#include "ECS/systems/unused.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>

namespace ECS {

void updateUnusedSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
    .ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<ECS::UnusedTag>(
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
      [&registry]
      (const auto& entity)
    {
      DCHECK(registry.valid(entity)
        && !registry.has<ECS::NeedToDestroyTag>(entity));
      registry.emplace<ECS::NeedToDestroyTag>(entity);
    });
}

} // namespace ECS
