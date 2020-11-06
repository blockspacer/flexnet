#include "testsCommon.h"

#if !defined(USE_GTEST_TEST)
#warning "use USE_GTEST_TEST"
// default
#define USE_GTEST_TEST 1
#endif // !defined(USE_GTEST_TEST)

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#ifndef __has_include
  static_assert(false, "__has_include not supported");
#else
#  if __has_include(<filesystem>)
#    include <filesystem>
     namespace fs = std::filesystem;
#  elif __has_include(<experimental/filesystem>)
#    include <experimental/filesystem>
     namespace fs = std::experimental::filesystem;
#  elif __has_include(<boost/filesystem.hpp>)
#    include <boost/filesystem.hpp>
     namespace fs = boost::filesystem;
#  endif
#endif

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/path_service.h>
#include <base/base64.h>
#include <base/base64url.h>
#include <base/command_line.h>
#include <base/lazy_instance.h>
#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/cpu.h>
#include <base/optional.h>
#include <base/path_service.h>
#include <base/time/time.h>
#include <base/memory/ptr_util.h>
#include <base/macros.h>
#include <base/stl_util.h>
#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/memory/scoped_refptr.h>
#include <base/single_thread_task_runner.h>
#include <base/threading/thread_task_runner_handle.h>
#include <base/numerics/checked_math.h>
#include <base/numerics/clamped_math.h>
#include <base/numerics/safe_conversions.h>
#include <base/i18n/icu_string_conversions.h>
#include <base/i18n/string_compare.h>
#include <base/stl_util.h>
#include <base/base_switches.h>
#include <base/command_line.h>
#include <base/containers/small_map.h>
#include <base/i18n/icu_util.h>
#include <base/i18n/rtl.h>
#include <base/allocator/partition_allocator/page_allocator.h>
#include <base/allocator/allocator_shim.h>
#include <base/allocator/buildflags.h>
#include <base/allocator/partition_allocator/partition_alloc.h>
#include <base/memory/scoped_refptr.h>
#include <base/i18n/rtl.h>
#include <base/stl_util.h>
#include <base/memory/ref_counted_memory.h>
#include <base/memory/read_only_shared_memory_region.h>
#include <base/stl_util.h>
#include <base/bind.h>
#include <base/memory/weak_ptr.h>
#include <base/threading/thread.h>
#include <base/logging.h>
#include <base/system/sys_info.h>
#include <base/synchronization/waitable_event.h>
#include <base/observer_list.h>
#include <base/synchronization/lock.h>
#include <base/threading/thread.h>
#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include <base/timer/timer.h>
#include <base/callback.h>
#include <base/bind.h>
#include <base/callback.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <base/memory/weak_ptr.h>
#include <base/message_loop/message_loop.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/callback.h>
#include <base/files/file_path.h>
#include <base/message_loop/message_loop.h>
#include <base/threading/thread.h>
#include <base/threading/thread_checker.h>
#include <base/feature_list.h>
#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/path_service.h>
#include <base/at_exit.h>
#include <base/command_line.h>
#include <base/message_loop/message_loop.h>
#include <base/run_loop.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <base/trace_event/memory_dump_manager.h>
#include <base/trace_event/heap_profiler.h>
#include <base/trace_event/heap_profiler_allocation_context_tracker.h>
#include <base/trace_event/heap_profiler_event_filter.h>
#include <base/sampling_heap_profiler/sampling_heap_profiler.h>
#include <base/sampling_heap_profiler/poisson_allocation_sampler.h>
#include <base/trace_event/malloc_dump_provider.h>
#include <base/trace_event/memory_dump_provider.h>
#include <base/trace_event/memory_dump_scheduler.h>
#include <base/trace_event/memory_infra_background_whitelist.h>
#include <base/trace_event/process_memory_dump.h>
#include <base/trace_event/trace_event.h>
#include <base/allocator/allocator_check.h>
#include <base/base_switches.h>
#include <base/threading/sequence_local_storage_map.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/threading/thread.h>
#include <base/threading/thread_task_runner_handle.h>
#include <base/process/process_handle.h>
#include <base/single_thread_task_runner.h>
#include <base/task_runner.h>
#include <base/task_runner_util.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/task/thread_pool/thread_pool_impl.h>
#include <base/threading/thread.h>
#include <base/threading/thread_local.h>
#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/threading/platform_thread.h>

#include <basis/ECS/tags.hpp>
#include <basis/ECS/helpers/prepend_child_entity.hpp>
#include <basis/ECS/helpers/foreach_child_entity.hpp>
#include <basis/ECS/helpers/view_child_entities.hpp>
#include <basis/ECS/helpers/remove_all_children_from_view.hpp>
#include <basis/ECS/helpers/remove_child_entity.hpp>
#include <basis/ECS/helpers/has_child_in_linked_list.hpp>

#include <flexnet/util/close_socket_unsafe.hpp>

TEST(ECSChildrenTest, test_hierarchies_in_ECS_model)
{
  class TestTypeTag{};

  using TagType = TestTypeTag;

  using FirstChildComponent = ECS::FirstChildInLinkedList<TagType>;
  using ChildrenComponent = ECS::ChildLinkedList<TagType>;
  /// \note we assume that size of all children can be stored in `size_t`
  using ChildrenSizeComponent = ECS::ChildLinkedListSize<TagType, size_t>;
  using ParentComponent = ECS::ParentEntity<TagType>;

  ECS::Registry registry;

  ECS::Entity parentId = registry.create();

  ECS::Entity childId = registry.create();

  DCHECK(!hasChildComponents<TagType>(registry, parentId));
  DCHECK(!hasParentComponents<TagType>(registry, childId));
  DCHECK(!hasChildComponents<TagType>(registry, childId));
  DCHECK(!hasParentComponents<TagType>(registry, parentId));

  {
    ECS::prependChildEntity<
        TagType
    >(
      REFERENCED(registry)
      , parentId
      , childId
    );

    DCHECK(hasChildComponents<TagType>(registry, childId));
    DCHECK(hasParentComponents<TagType>(registry, parentId));
    DCHECK(!hasChildComponents<TagType>(registry, parentId));
    DCHECK(!hasParentComponents<TagType>(registry, childId));

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 1);

    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, registry.create()));

    DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).nextId, ECS::NULL_ENTITY);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).prevId, ECS::NULL_ENTITY);
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 1);
    DCHECK_EQ(iteratedEntities[0], childId);
  }

  ECS::Entity childTwoId = registry.create();

  {
    ECS::prependChildEntity<
      TagType
    >(
      REFERENCED(registry)
      , parentId
      , childTwoId
    );

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childTwoId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 2);

    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childId));
    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childTwoId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, registry.create()));

    DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).nextId, ECS::NULL_ENTITY);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).prevId, childTwoId);

    DCHECK_EQ(registry.get<ParentComponent>(childTwoId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childTwoId).nextId, childId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childTwoId).prevId, ECS::NULL_ENTITY);
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 2);
    DCHECK_EQ(iteratedEntities[0], childTwoId);
    DCHECK_EQ(iteratedEntities[1], childId);
  }

  ECS::Entity childThreeId = registry.create();

  {
    ECS::prependChildEntity<
      TagType
    >(
      REFERENCED(registry)
      , parentId
      , childThreeId
    );

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childThreeId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 3);

    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childId));
    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childTwoId));
    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childThreeId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, registry.create()));

    DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).nextId, ECS::NULL_ENTITY);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).prevId, childTwoId);

    DCHECK_EQ(registry.get<ParentComponent>(childTwoId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childTwoId).nextId, childId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childTwoId).prevId, childThreeId);

    DCHECK_EQ(registry.get<ParentComponent>(childThreeId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childThreeId).nextId, childTwoId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childThreeId).prevId, ECS::NULL_ENTITY);
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 3);
    DCHECK_EQ(iteratedEntities[0], childThreeId);
    DCHECK_EQ(iteratedEntities[1], childTwoId);
    DCHECK_EQ(iteratedEntities[2], childId);
  }

  {
    bool removeOk
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , childTwoId
      );
    DCHECK(removeOk);

    DCHECK(!registry.has<ParentComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childThreeId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 2);

    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childId));
    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childThreeId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, childTwoId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, registry.create()));

    DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).nextId, ECS::NULL_ENTITY);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).prevId, childThreeId);

    DCHECK_EQ(registry.get<ParentComponent>(childThreeId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childThreeId).nextId, childId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childThreeId).prevId, ECS::NULL_ENTITY);
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 2);
    DCHECK_EQ(iteratedEntities[0], childThreeId);
    DCHECK_EQ(iteratedEntities[1], childId);
  }

  {
    bool removeOk
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , childTwoId
      );
    DCHECK(!removeOk);
  }

  {
    ECS::Entity tmpId = registry.create();

    bool removeOk
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , tmpId
      );
    DCHECK(!removeOk);
  }

  {
    bool removeOk
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , childThreeId
      );
    DCHECK(removeOk);

    DCHECK(!registry.has<ParentComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));

    DCHECK(!registry.has<ParentComponent>(childThreeId));
    DCHECK(!registry.has<ChildrenComponent>(childThreeId));
    DCHECK(!registry.has<ChildrenComponent>(childThreeId));

    DCHECK(hasChildInLinkedList<TagType>(registry, parentId, childId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, childTwoId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, childThreeId));
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, registry.create()));

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 1);

    DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).nextId, ECS::NULL_ENTITY);
    DCHECK_EQ(registry.get<ChildrenComponent>(childId).prevId, ECS::NULL_ENTITY);
  }

  {
    ECS::Entity tmpParentId = registry.create();
    ECS::Entity tmpChildId = registry.create();

    bool removeOk1
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , tmpParentId
        , tmpChildId
      );
    DCHECK(!removeOk1);

    bool removeOk2
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , tmpParentId
        , childId
      );
    DCHECK(!removeOk2);

    ECS::prependChildEntity<
      TagType
    >(
      REFERENCED(registry)
      , tmpParentId
      , tmpChildId
    );

    DCHECK_EQ(registry.get<FirstChildComponent>(tmpParentId).firstId, tmpChildId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(tmpParentId).size, 1);

    bool removeOk3
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , tmpChildId
      );
    DCHECK(!removeOk3);
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 1);
    DCHECK_EQ(iteratedEntities[0], childId);
  }

  {
    bool removeOk
      = ECS::removeChildEntity<
        TagType
      >(
        REFERENCED(registry)
        , parentId
        , childId
      );
    DCHECK(removeOk);

    DCHECK(!registry.has<ParentComponent>(childId));
    DCHECK(!registry.has<ChildrenComponent>(childId));
    DCHECK(!registry.has<ChildrenComponent>(childId));

    DCHECK(!registry.has<ParentComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));
    DCHECK(!registry.has<ChildrenComponent>(childTwoId));

    DCHECK(!registry.has<ParentComponent>(childThreeId));
    DCHECK(!registry.has<ChildrenComponent>(childThreeId));
    DCHECK(!registry.has<ChildrenComponent>(childThreeId));

    DCHECK(!registry.has<FirstChildComponent>(parentId));
    DCHECK(!registry.has<ChildrenSizeComponent>(parentId));
  }

  {
    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 0);
  }

  {
    DCHECK(!hasChildInLinkedList<TagType>(registry, parentId, ECS::NULL_ENTITY));
    DCHECK(!hasChildInLinkedList<TagType>(registry, ECS::NULL_ENTITY, childId));
    DCHECK(!hasChildComponents<TagType>(registry, ECS::NULL_ENTITY));
    DCHECK(!hasParentComponents<TagType>(registry, ECS::NULL_ENTITY));
  }

  {
    DCHECK(!registry.has<ChildrenSizeComponent>(parentId));

    ECS::prependChildEntity<
      TagType
    >(
      REFERENCED(registry)
      , parentId
      , childThreeId
    );

    DCHECK_EQ(registry.get<FirstChildComponent>(parentId).firstId, childThreeId);
    DCHECK_EQ(registry.get<ChildrenSizeComponent>(parentId).size, 1);

    CREATE_ECS_TAG(Internal_hasChildInLinkedListrenTag);

    registry.emplace<Internal_hasChildInLinkedListrenTag>(parentId);

    removeAllChildrenFromView<
      TagType
    >(
      REFERENCED(registry)
      , ECS::include<
          Internal_hasChildInLinkedListrenTag
        >
      , ECS::exclude<>
    );

    std::vector<ECS::Entity> iteratedEntities{};

    ECS::foreachChildEntity<TagType>(
      REFERENCED(registry)
      , parentId
      , base::BindRepeating(
        [
        ](
          std::vector<ECS::Entity>& iterated
          , ECS::Registry& registry
          , ECS::Entity parentId
          , ECS::Entity childId
        ){
          DCHECK_EQ(registry.get<ParentComponent>(childId).parentId, parentId);

          iterated.push_back(childId);
        }
        , REFERENCED(iteratedEntities)
      )
    );

    DCHECK_EQ(iteratedEntities.size(), 0);

    DCHECK(!registry.has<ChildrenSizeComponent>(parentId));
  }
}
