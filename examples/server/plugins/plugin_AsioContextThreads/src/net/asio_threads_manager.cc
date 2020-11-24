#include "net/asio_threads_manager.hpp" // IWYU pragma: associated

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/guid.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/feature_list.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <base/trace_event/memory_dump_manager.h>
#include <base/trace_event/heap_profiler.h>
#include <base/trace_event/heap_profiler_allocation_context_tracker.h>
#include <base/trace_event/heap_profiler_event_filter.h>
#include <base/sampling_heap_profiler/sampling_heap_profiler.h>
#include <base/sampling_heap_profiler/poisson_allocation_sampler.h>
#include <base/sampling_heap_profiler/module_cache.h>
#include <base/profiler/frame.h>
#include <base/trace_event/malloc_dump_provider.h>
#include <base/trace_event/memory_dump_provider.h>
#include <base/trace_event/memory_dump_scheduler.h>
#include <base/trace_event/memory_infra_background_whitelist.h>
#include <base/trace_event/process_memory_dump.h>
#include <base/trace_event/trace_event.h>

#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>
#include <basis/scoped_checks.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/ECS/sequence_local_context.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

AsioThreadsManager::AsioThreadsManager()
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

AsioThreadsManager::~AsioThreadsManager()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  for(size_t i = 0; i < asio_threads_.size(); i++) {
    std::unique_ptr<AsioThreadType>& asio_thread
      = asio_threads_[i];
    DCHECK(asio_thread);
    DCHECK(!asio_thread->IsRunning());
  }
}

void AsioThreadsManager::stopThreads()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  for(size_t i = 0; i < asio_threads_.size(); i++) {
    std::unique_ptr<AsioThreadType>& asio_thread
      = asio_threads_[i];
    DCHECK(asio_thread);
    DCHECK(asio_thread->IsRunning());
    DVLOG(99)
      << "stopping asio_thread...";
    /// \note Start/Stop not thread-safe
    asio_thread->Stop();
    // wait loop stopped
    DCHECK(!asio_thread->IsRunning());
    DVLOG(99)
      << "stopped asio_thread...";
  }
}

void AsioThreadsManager::startThreads(
  const size_t threadsNum
  , boost::asio::io_context& ioc)
{
  LOG_CALL(DVLOG(99))
    << "creating "
    << threadsNum
    << " asio threads";

  DCHECK_RUN_ON(&sequence_checker_);

  asio_threads_.reserve(threadsNum);

  for(size_t i = 0; i < threadsNum; i++)
  {
    std::unique_ptr<AsioThreadType> asio_thread
      = std::make_unique<AsioThreadType>(
          "AsioThread_" + std::to_string(i));

    DCHECK(asio_thread);
    ::base::Thread::Options options;
    asio_thread->StartWithOptions(options);
    asio_thread->WaitUntilThreadStarted();

    asio_task_runners_.push_back(
      asio_thread->task_runner());

    DVLOG(99)
      << "created asio thread...";

    /// \note whole thread is dedicated for asio ioc
    /// we create one |SequencedTaskRunner| per one |base::Thread|
    DCHECK(asio_task_runners_[i]);

    asio_threads_.push_back(
      ::base::rvalue_cast(asio_thread));

    asio_task_runners_[i]->PostTask(
      FROM_HERE
        , ::base::bindCheckedRepeating(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &AsioThreadsManager::runIoc
          , ::base::Unretained(this)
          , REFERENCED(ioc)
        )
    );
  }
}

void AsioThreadsManager::runIoc(boost::asio::io_context& ioc)
{
  LOG_CALL(DVLOG(99));

  DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(runIoc);

  if(ioc.stopped()) // io_context::stopped is thread-safe
  {
    LOG(INFO)
      << "skipping update of stopped io context";
    return;
  }

  // we want to loop |ioc| forever
  // i.e. until manual termination
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard
    = boost::asio::make_work_guard(ioc);

  /// \note loops forever (if has work) and
  /// blocks |task_runner->PostTask| for that thread!
  ioc.run();

  LOG(INFO)
    << "stopped io context thread";
}

} // namespace backend
