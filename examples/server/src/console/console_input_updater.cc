#include "console/console_input_updater.hpp" // IWYU pragma: associated
#include "console/console_feature_list.hpp"

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

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

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
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
#include <basis/ECS/sequence_local_context.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

ConsoleInputUpdater::ConsoleInputUpdater(
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner
  , HandleConsoleInputCb&& consoleInputCb)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , periodicConsoleTaskRunner_(periodicConsoleTaskRunner)
  , consoleInputCb_(base::rvalue_cast(consoleInputCb))
  , periodicTaskExecutor_(
      base::BindRepeating(
        &ConsoleInputUpdater::update
        /// \note callback may be skipped if `WeakPtr` becomes invalid
        , weakSelf()
      )
    )
{
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

ConsoleInputUpdater::~ConsoleInputUpdater()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void ConsoleInputUpdater::update() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicConsoleTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_consoleInputCb_);

  DCHECK(periodicConsoleTaskRunner_->RunsTasksInCurrentSequence());

  std::string line;

  /// \todo std::getline blocks without timeout, so add timeout like in
  /// github.com/throwaway-github-account-alex/coding-exercise/blob/9ccd3d04ffa5569a2004ee195171b643d252514b/main.cpp#L12
  /// or stackoverflow.com/a/34994150
  /// i.e. use STDIN_FILENO and poll in Linux
  /// In Windows, you will need to use timeSetEvent or WaitForMultipleEvents
  /// or need the GetStdHandle function to obtain a handle
  /// to the console, then you can use WaitForSingleObject
  /// to wait for an event to happen on that handle, with a timeout
  /// see stackoverflow.com/a/21749034
  /// see stackoverflow.com/questions/40811438/input-with-a-timeout-in-c
  std::getline(std::cin, line);

  DVLOG(99)
    << "console input: "
    << line;

  DCHECK(consoleInputCb_);
  consoleInputCb_.Run(line);
}

MUST_USE_RETURN_VALUE
basis::PeriodicTaskExecutor&
  ConsoleInputUpdater::periodicTaskExecutor() NO_EXCEPTION
{
  DCHECK_RUN_ON_ANY_THREAD_SCOPE(fn_periodicTaskExecutor);

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicTaskExecutor_);
  return periodicTaskExecutor_;
}

} // namespace backend
