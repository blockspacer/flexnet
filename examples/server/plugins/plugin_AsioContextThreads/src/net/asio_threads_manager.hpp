#pragma once

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
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
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

class AsioThreadsManager
{
public:
  using AsioThreadType
    = ::base::Thread;

public:
  SET_WEAK_SELF(AsioThreadsManager)

  AsioThreadsManager()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  ~AsioThreadsManager()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  // creates |threadsNum| threads that will
  // invoke |run| method from |ioc|
  void startThreads(
    const size_t threadsNum
    , boost::asio::io_context& ioc)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  void stopThreads()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  const std::vector<
    std::unique_ptr<AsioThreadType>
  >& threads() const
    PUBLIC_METHOD_RUN_ON(&sequence_checker_)
  {
    return asio_threads_;
  }

private:
  void runIoc(boost::asio::io_context& ioc);

private:
  SET_WEAK_POINTERS(AsioThreadsManager);

  /// \note usually you do not want to post tasks
  /// on that task runner
  /// i.e. use asio based task runners and sequences!
  std::vector<
    scoped_refptr<::base::SequencedTaskRunner>
  > asio_task_runners_
    GUARDED_BY(sequence_checker_);

  std::vector<
    std::unique_ptr<AsioThreadType>
  > asio_threads_
    GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(AsioThreadsManager);
};

} // namespace backend
