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
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
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

// Creates `periodicTaskExecutor_` that will
// run `update()` on `periodicAsioTaskRunner`
class NetworkEntityUpdater
{
 public:
  using IoContext
   = boost::asio::io_context;

  NetworkEntityUpdater(
    scoped_refptr<::base::SequencedTaskRunner> periodicAsioTaskRunner
    , ECS::SafeRegistry& registry_
    , boost::asio::io_context& ioc_);

  ~NetworkEntityUpdater();

  // Posts task to strand associated with registry
  // that will update ECS systems
  void update() NO_EXCEPTION
    PUBLIC_METHOD_RUN_ON(periodicAsioTaskRunner_.get());

  MUST_USE_RETURN_VALUE
  ::basis::PeriodicTaskExecutor& periodicTaskExecutor() NO_EXCEPTION;

  SET_WEAK_SELF(NetworkEntityUpdater)

 private:
  void updateOnRegistryThread() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(registry_);

 private:
  SET_WEAK_POINTERS(NetworkEntityUpdater);

  ECS::SafeRegistry& registry_;

  // The io_context is required for all I/O
  IoContext& ioc_;

  // Task sequence used to update `network-ECS`
  scoped_refptr<::base::SequencedTaskRunner> periodicAsioTaskRunner_;

  /// \note will stop periodic timer on scope exit
  ::basis::PeriodicTaskExecutor periodicTaskExecutor_;

  // number of `update` tasks posted to `registry_.taskRunner`
  std::atomic<size_t> numQueuedUpdateTasks_;

  size_t warnBigUpdateQueueSize_
    GUARDED_BY(registry_);

  int warnBigUpdateQueueFreqMs_
    GUARDED_BY(registry_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(NetworkEntityUpdater);
};

} // namespace backend
