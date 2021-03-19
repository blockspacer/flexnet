#include "main_loop_registry.hpp" // IWYU pragma: associated

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/metrics/histogram_functions.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <basic/rvalue_cast.h>
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

#include <basic/annotations/guard_annotations.h>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/task/task_util.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

MainLoopRegistry*
  MainLoopRegistry::GetInstance()
{
  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  /// Singleton itself thread-safe.
  /// The underlying Type must of course be
  /// thread-safe if you want to use it concurrently.
  return ::base::Singleton<MainLoopRegistry>::get();
}

} // namespace backend
