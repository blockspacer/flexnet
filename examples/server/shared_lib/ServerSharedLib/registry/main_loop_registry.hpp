#pragma once

#include "state/app_state.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>

#include <base/memory/singleton.h>
#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/optional.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/network_registry.hpp>
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
#include <basis/scoped_sequence_context_var.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <memory>
#include <chrono>

namespace backend {

// Guaranteed to be valid while `base::RunLoop.Run()` is running.
class MainLoopRegistry {
 public:
  using RegistryType
    = entt::registry;

  /// \note Every call incurs some overhead
  /// to check whether the object has already been initialized.
  /// You may wish to cache the result of get(); it will not change.
  static MainLoopRegistry* GetInstance();

  RegistryType& registry()
  {
    // expected to be used only from thread with `base::RunLoop.Run()`
    DCHECK(base::RunLoop::IsRunningOnCurrentThread());

    return registry_;
  }

  const RegistryType& registry() const
  {
    // expected to be used only from thread with `base::RunLoop.Run()`
    DCHECK(base::RunLoop::IsRunningOnCurrentThread());

    return registry_;
  }

 private:
  // private due to singleton
  MainLoopRegistry() = default;

  // private due to singleton
  ~MainLoopRegistry() = default;

  friend struct base::DefaultSingletonTraits<MainLoopRegistry>;

 private:
  /// \note registry is not thread-safe,
  /// so use it only from one thread or sequence.
  RegistryType registry_;

  DISALLOW_COPY_AND_ASSIGN(MainLoopRegistry);
};

} // namespace backend
