#include "basis/ECS/network_registry.hpp"

#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/helpers/foreach_child_entity.hpp>
#include <flexnet/ECS/helpers/remove_child_entity.hpp>
#include <flexnet/ECS/components/child_linked_list.hpp>
#include <flexnet/ECS/components/child_linked_list_size.hpp>
#include <flexnet/ECS/components/first_child_in_linked_list.hpp>
#include <flexnet/ECS/components/parent_entity.hpp>

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

// For each unused entity that have children:
// 1. Marks all children with `ECS::NeedToDestroyTag`
// 2. Removes all children from entity.
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.

template <typename TagT>
void updateUnusedChildList(
  ECS::NetworkRegistry& net_registry)
{
  using FirstChildComponent = ECS::FirstChildInLinkedList<TagT>;
  using ChildrenComponent = ECS::ChildLinkedList<TagT>;
  /// \note we assume that size of all children can be stored in `size_t`
  using ChildrenSizeComponent = ECS::ChildLinkedListSize<TagT, size_t>;
  using ParentComponent = ECS::ParentEntity<TagT>;

  DCHECK_RUN_ON_NET_REGISTRY(&net_registry);

  auto registry_group
    = net_registry->view<
        // unused entities
        ECS::UnusedTag
        // entities that have children
        , FirstChildComponent
      >(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >
      );

  if(registry_group.size()) {
    UMA_HISTOGRAM_COUNTS_1000("ECS.unusedChildListBatches",
      // How many entities became unused in single pass.
      registry_group.size());
  }

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " found unused entities with children, total number: "
      << registry_group.size();
  }
#endif // NDEBUG

  for(const ECS::Entity& entity_id: registry_group)
  {
    DCHECK(net_registry->valid(entity_id));

    DVLOG(99)
      << " found unused entity with children. Entity id: "
      << entity_id;

    DCHECK(net_registry->has<FirstChildComponent>(entity_id));
    DCHECK(net_registry->has<ChildrenSizeComponent>(entity_id));

    // destroy children of unused entities
    /// \todo Now children may be freed before parents.
    /// How to preserve order without affecting performance?
    ECS::foreachChildEntity<TagT>(
      REFERENCED(net_registry.registryUnsafe())
      , entity_id
      , base::BindRepeating(
        [
        ](
          ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK(!registry.has<ECS::NeedToDestroyTag>(childId));
          registry.emplace<ECS::NeedToDestroyTag>(childId);

          DVLOG(99)
            << " removed child entity "
            << childId
            << " from parent entity "
            << parentId;

          DCHECK(registry.has<ChildrenComponent>(childId));

          bool removedOk
            = ECS::removeChildEntity<TagT>(
              REFERENCED(registry)
              , parentId
              , childId
            );
          DCHECK(removedOk);

          DCHECK(!registry.has<ParentComponent>(childId));
        }
      )
    );

    DCHECK(!net_registry->has<FirstChildComponent>(entity_id));
    DCHECK(!net_registry->has<ChildrenSizeComponent>(entity_id));
  };
}

} // namespace ECS
