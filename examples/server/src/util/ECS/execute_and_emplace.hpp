#pragma once

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>

#include <basis/checked_optional.hpp>
#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/asio_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

template<typename ComponentType>
using IterateRegistryCb
  = base::RepeatingCallback<
      void(ECS::Entity, ECS::Registry& registry)>;

// Use it if you want to perform some task with entity and then mark entity
// as `already processed` (to avoid processing twice).
//
// ALGORITHM
//
// 1 step:
// Executes provided callback (per each entity in `entities view`).
// 2 step:
// Adds component into each entity in `entities view`
// using provided ECS registry.
template<
  // component to emplace (if not exists)
  typename ComponentType
  // filtered `entities view`
  , typename EntitiesViewType
  // arguments for component construction
  , class... Args
>
static void executeAndEmplace(
  IterateRegistryCb<ComponentType> taskCb
  , ECS::AsioRegistry& asioRegistry
  , EntitiesViewType& ecsView
  , Args&&... args)
{
  LOG_CALL(DVLOG(99));

  DCHECK(asioRegistry.running_in_this_thread());

  ecsView
    .each(
      [&taskCb, &asioRegistry, ... args = std::forward<Args>(args)]
      (const auto& entity
       , const auto& component)
    {
      taskCb.Run(entity, (*asioRegistry));
    });

  asioRegistry->insert<ComponentType>(
    ecsView.begin()
    , ecsView.end()
    , std::forward<Args>(args)...);

#if DCHECK_IS_ON()
  // sanity check due to bug in old entt versions
  // checks `registry.insert(begin, end)`
  ecsView
    .each(
      [&asioRegistry, ... args = std::forward<Args>(args)]
      (const auto& entity
       , const auto& component)
    {
      // inserted component must exist
      DCHECK(asioRegistry->has<ComponentType>(entity));
    });
#endif // DCHECK_IS_ON()
}

} // namespace backend
