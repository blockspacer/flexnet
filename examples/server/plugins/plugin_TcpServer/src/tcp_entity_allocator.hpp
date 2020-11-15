#pragma once

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

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
#include <basis/scoped_checks.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
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
#include <basis/strong_types/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

class TcpEntityAllocator
{
 public:
  TcpEntityAllocator(
    ECS::NetworkRegistry& netRegistry);

  ~TcpEntityAllocator();

  /// \note creates ECS entity used as `asio connection`,
  /// sets common components and performs checks
  /// (if entity was re-used from cache some components must be reset)
  //
  // MOTIVATION
  //
  // Each component type must be reset if you want to re-use it
  // (i.e. if you want to use `cache` to avoid allocations).
  // If you manually registered component in `allowed` list,
  // then we can assume that component can be re-used.
  // We prohibit any `unknown` types of entities that can be re-used.
  MUST_USE_RETURN_VALUE
  ECS::Entity allocateTcpEntity() NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(&netRegistryRef_->taskRunner());

  SET_WEAK_SELF(TcpEntityAllocator)

 private:
  SET_WEAK_POINTERS(TcpEntityAllocator);

private:
  util::UnownedRef<ECS::NetworkRegistry> netRegistryRef_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(netRegistryRef_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(TcpEntityAllocator);
};

} // namespace backend
