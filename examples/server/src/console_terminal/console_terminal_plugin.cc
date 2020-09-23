#include "console_terminal/console_terminal_plugin.hpp" // IWYU pragma: associated

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

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

ConsoleTerminalPlugin::ConsoleTerminalPlugin()
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , periodicConsoleTaskRunner_(
      base::ThreadPool::GetInstance()->
        CreateSequencedTaskRunnerWithTraits(
          base::TaskTraits{
            base::TaskPriority::BEST_EFFORT
            , base::MayBlock()
            , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
          }
        ))
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  if(base::FeatureList::IsEnabled(kFeatureConsoleTerminal))
  {
    consoleTerminal_.emplace(periodicConsoleTaskRunner_);
  }
}

ConsoleTerminalPlugin::~ConsoleTerminalPlugin()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  // Do not forget to call `unload()`.
  DCHECK(!consoleTerminal_);
}

ConsoleTerminalPlugin::VoidPromise ConsoleTerminalPlugin::load(
  ConsoleInputUpdater::HandleConsoleInputCb consoleInputCb)
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::FeatureList::IsEnabled(kFeatureConsoleTerminal));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicConsoleTaskRunner_));

  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK(consoleInputCb);

  scoped_refptr<base::SequencedTaskRunner> runnerCopy
    = periodicConsoleTaskRunner_;

  ConsoleInputUpdater::HandleConsoleInputCb cbCopy
    = consoleInputCb;

  return consoleTerminal_->promiseEmplaceAndStart
      <
        scoped_refptr<base::SequencedTaskRunner>
        , ConsoleInputUpdater::HandleConsoleInputCb
      >
      (FROM_HERE
        , "PeriodicConsoleExecutor" // debug name
        , base::rvalue_cast(runnerCopy)
        , base::rvalue_cast(cbCopy)
      )
  .ThenHere(FROM_HERE
    , base::BindOnce(
        &ConsoleTerminalPlugin::onLoaded
        , base::Unretained(this)
    )
  );
}

ConsoleTerminalPlugin::VoidPromise ConsoleTerminalPlugin::unload()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK(base::FeatureList::IsEnabled(kFeatureConsoleTerminal));

  // prolong lifetime of shared object because we free `consoleTerminal_` below
  auto sharedPromise = consoleTerminal_->promiseDeletion();

  /// \note posts async-task that will delete object in sequence-local-context
  consoleTerminal_.reset();

  return sharedPromise
  .ThenHere(FROM_HERE
    , base::BindOnce(
        &ConsoleTerminalPlugin::onUnloaded
        , base::Unretained(this)
    )
  );
}

} // namespace backend
