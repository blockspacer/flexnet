#pragma once

#include "console/console_input_updater.hpp"
#include "console/console_terminal_on_sequence.hpp"
#include "console/console_feature_list.hpp"
#include "net/network_entity_updater_on_sequence.hpp"
#include "util/ECS/execute_and_emplace.hpp"

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

class ConsoleTerminalPlugin
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  ConsoleTerminalPlugin();

  ~ConsoleTerminalPlugin();

  // blocking construction of object in sequence-local-storage
  VoidPromise load(
    ConsoleInputUpdater::HandleConsoleInputCb consoleInputCb);

  VoidPromise unload();

  SET_WEAK_SELF(ConsoleTerminalPlugin)

private:
  void onLoaded()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_RUN_ON(&sequence_checker_);

    DCHECK(consoleTerminal_);
  }

  void onUnloaded()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_RUN_ON(&sequence_checker_);

    DCHECK(!consoleTerminal_);
  }

 private:
  SET_WEAK_POINTERS(ConsoleTerminalPlugin);

  // Task sequence used to update text input from console terminal.
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(periodicConsoleTaskRunner_));

  // On scope exit will schedule destruction (from sequence-local-context),
  // so use `base::Optional` to control scope i.e. control lifetime.
  base::Optional<ConsoleTerminalOnSequence> consoleTerminal_
    GUARDED_BY(&sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ConsoleTerminalPlugin);
};

} // namespace backend
