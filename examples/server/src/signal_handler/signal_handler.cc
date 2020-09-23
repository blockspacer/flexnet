#include "example_server.hpp" // IWYU pragma: associated
#include "console_terminal/console_input_updater.hpp"
#include "console_terminal/console_terminal_on_sequence.hpp"
#include "console_terminal/console_feature_list.hpp"

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

#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/metrics/histogram_functions.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
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

SignalHandler::SignalHandler(
  ::boost::asio::io_context& ioc
  , base::OnceClosure&& quitCb)
  : signals_set_(
      /// \note will not handle signals if ioc stopped
      ioc, SIGINT, SIGTERM)
    , quitCb_(base::rvalue_cast(quitCb))
    , signalsRecievedCount_(0)
{
  DETACH_FROM_SEQUENCE(sequence_checker_);

#if defined(SIGQUIT)
  signals_set_.add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  signals_set_.async_wait(
    ::std::bind(
      &SignalHandler::handleQuitSignal
      , this
      , std::placeholders::_1
      , std::placeholders::_2)
  );
}

void SignalHandler::handleQuitSignal(
  ::boost::system::error_code const&
  , int)
{
  DCHECK_RUN_ON_ANY_THREAD_SCOPE(FUNC_GUARD(handleQuitSignal));

  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(quitCb_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(signalsRecievedCount_));

  DVLOG(9)
    << "got stop signal";

  signalsRecievedCount_.store(
    signalsRecievedCount_.load() + 1);

  UMA_HISTOGRAM_COUNTS_1000("App.Signals.RecievedCount"
    , signalsRecievedCount_.load());

  // User pressed `stop` many times (possibly due to hang).
  // Assume he really wants to terminate application,
  // so call `std::exit` to exit immediately.
  if(signalsRecievedCount_.load() > 3) {
    LOG(ERROR)
      << "unable to send stop signal gracefully,"
         " forcing application termination";
    std::exit(EXIT_SUCCESS);
    NOTREACHED();
  }
  // Unable to terminate application twice.
  else if(signalsRecievedCount_.load() > 1) {
    LOG(WARNING)
      << "application already recieved stop signal, "
         "ignoring";
    return;
  }

  base::rvalue_cast(quitCb_).Run();
}

SignalHandler::~SignalHandler()
{
  DCHECK_RUN_ON(&sequence_checker_);
}

} // namespace backend
