#include "basis/ECS/network_registry.hpp"

#include <basis/ECS/tags.hpp>
#include <basis/ECS/helpers/foreach_child_entity.hpp>
#include <basis/ECS/helpers/view_child_entities.hpp>
#include <basis/ECS/helpers/remove_all_children_from_view.hpp>
#include <basis/ECS/helpers/remove_parent_components.hpp>
#include <basis/ECS/helpers/remove_child_components.hpp>
#include <basis/ECS/components/child_linked_list.hpp>
#include <basis/ECS/components/child_linked_list_size.hpp>
#include <basis/ECS/components/first_child_in_linked_list.hpp>
#include <basis/ECS/components/parent_entity.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

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

template <typename TagT>
auto groupParentsWithUnusedChilds(
  ECS::NetworkRegistry& net_registry)
{
  using FirstChildComponent = ECS::FirstChildInLinkedList<TagT>;

  DCHECK_RUN_ON_NET_REGISTRY(&net_registry);

  return
    net_registry->view<
      // unused entities
      ECS::UnusedTag
      // entities that have children
      , FirstChildComponent
    >(
      entt::exclude<
        // entity in destruction
        ECS::NeedToDestroyTag
        // entity not fully created
        , ECS::DelayedConstruction
      >
    );
}

// For each unused entity that have children:
// 1. Marks all children with `ECS::NeedToDestroyTag`
// 2. Removes all children from entity.
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
template <typename TagType>
void updateUnusedChildList(
  ECS::NetworkRegistry& net_registry)
{
  using FirstChildComponent = ECS::FirstChildInLinkedList<TagType>;
  using ChildrenComponent = ECS::ChildLinkedList<TagType>;
  /// \note we assume that size of all children can be stored in `size_t`
  using ChildrenSizeComponent = ECS::ChildLinkedListSize<TagType, size_t>;
  using ParentComponent = ECS::ParentEntity<TagType>;

  DCHECK_RUN_ON_NET_REGISTRY(&net_registry);

  auto registry_group
    = groupParentsWithUnusedChilds<TagType>(net_registry);

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

  // To work around issues during iterations we store aside
  // the entities and the components to be removed
  // and perform the operations at the end of the iteration.
  CREATE_ECS_TAG(Internal_ChildrenToDestroy);

  for(const ECS::Entity& parentEntityId: registry_group)
  {
    DVLOG(99)
      << " found unused entity with children. Entity id: "
      << parentEntityId;

    ECS::foreachChildEntity<TagType>(
      REFERENCED(net_registry.registryUnsafe())
      , parentEntityId
      , ::base::BindRepeating(
        [
        ](
          ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DVLOG(99)
            << " removed child entity "
            << childId
            << " from parent entity "
            << parentId;

          DCHECK(!registry.has<Internal_ChildrenToDestroy>(childId));
          registry.emplace<Internal_ChildrenToDestroy>(childId);
        }
      )
    );
  };

  /// \note create new group to avoid iterator invalidation
  for(const ECS::Entity& childId:
    net_registry->view<Internal_ChildrenToDestroy>())
  {
    DCHECK(!net_registry->has<ECS::NeedToDestroyTag>(childId));
    net_registry->emplace<ECS::NeedToDestroyTag>(childId);

    net_registry->remove<Internal_ChildrenToDestroy>(childId);
  }

  removeAllChildrenFromView<
    TagType
  >(
    REFERENCED(net_registry.registryUnsafe())
    , ECS::include<
        ECS::UnusedTag
      >
    , ECS::exclude<
        // entity in destruction
        ECS::NeedToDestroyTag
        // entity not fully created
        , ECS::DelayedConstruction
      >
  );
}

} // namespace ECS
