#include "signal_handler.hpp" // IWYU pragma: associated

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

#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <memory>
#include <chrono>

namespace plugin {
namespace signal_handler {

static const int kSigMin = 1;

#if !defined(SIGSYS)
  #error "SIGSYS not defined"
#endif // defined(SIGQUIT)

static const int kSigMax = SIGSYS;

#define SIGNAL_NAME_PAIR(X) \
  X,#X

static std::map<int, std::string> gSignalToName{
  {SIGNAL_NAME_PAIR(SIGHUP)}
  , {SIGNAL_NAME_PAIR(SIGINT)}
  , {SIGNAL_NAME_PAIR(SIGQUIT)}
  , {SIGNAL_NAME_PAIR(SIGILL)}
  , {SIGNAL_NAME_PAIR(SIGTRAP)}
  , {SIGNAL_NAME_PAIR(SIGABRT)}
  , {SIGNAL_NAME_PAIR(SIGBUS)}
  , {SIGNAL_NAME_PAIR(SIGFPE)}
  , {SIGNAL_NAME_PAIR(SIGKILL)}
  , {SIGNAL_NAME_PAIR(SIGUSR1)}
  , {SIGNAL_NAME_PAIR(SIGSEGV)}
  , {SIGNAL_NAME_PAIR(SIGUSR2)}
  , {SIGNAL_NAME_PAIR(SIGPIPE)}
  , {SIGNAL_NAME_PAIR(SIGALRM)}
  , {SIGNAL_NAME_PAIR(SIGTERM)}
  , {SIGNAL_NAME_PAIR(SIGSTKFLT)}
  , {SIGNAL_NAME_PAIR(SIGCHLD)}
  , {SIGNAL_NAME_PAIR(SIGCONT)}
  , {SIGNAL_NAME_PAIR(SIGSTOP)}
  , {SIGNAL_NAME_PAIR(SIGTSTP)}
  , {SIGNAL_NAME_PAIR(SIGTTIN)}
  , {SIGNAL_NAME_PAIR(SIGTTOU)}
  , {SIGNAL_NAME_PAIR(SIGURG)}
  , {SIGNAL_NAME_PAIR(SIGXCPU)}
  , {SIGNAL_NAME_PAIR(SIGXFSZ)}
  , {SIGNAL_NAME_PAIR(SIGVTALRM)}
  , {SIGNAL_NAME_PAIR(SIGPROF)}
  , {SIGNAL_NAME_PAIR(SIGWINCH)}
  , {SIGNAL_NAME_PAIR(SIGIO)}
  , {SIGNAL_NAME_PAIR(SIGPWR)}
  , {SIGNAL_NAME_PAIR(SIGSYS)}
};

static std::string getSignalName(int signum)
{
  if (signum >= kSigMin && signum <= kSigMax)
  {
    // make sure you registered all basic signals in `gSignalToName`
    DCHECK(gSignalToName.find(signum) != gSignalToName.end());
  }

  if(gSignalToName.find(signum) != gSignalToName.end())
  {
    return gSignalToName[signum];
  }

  return std::string{"Unknown signal "} + std::to_string(signum);
}

SignalHandler::SignalHandler(
  ::boost::asio::io_context& ioc
  , ::base::OnceClosure&& quitCb)
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

  signalCallbacks_[SIGQUIT]
    = ::base::bindCheckedRepeating(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &SignalHandler::handleQuitSignal
        , ::base::Unretained(this)
    );

#if defined(SIGINT)
  signals_set_.add(SIGINT);
#else
  #error "SIGINT not defined"
#endif // defined(SIGQUIT)

  signalCallbacks_[SIGINT]
    = ::base::bindCheckedRepeating(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &SignalHandler::handleQuitSignal
        , ::base::Unretained(this)
    );

#if defined(SIGQUIT)
  signals_set_.add(SIGTERM);
#else
  #error "SIGTERM not defined"
#endif // defined(SIGTERM)

  signalCallbacks_[SIGTERM]
    = ::base::bindCheckedRepeating(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &SignalHandler::handleQuitSignal
        , ::base::Unretained(this)
    );

  signals_set_.async_wait(
    ::std::bind(
      &SignalHandler::handleSignal
      , this
      , std::placeholders::_1
      , std::placeholders::_2)
  );
}

void SignalHandler::handleSignal(
  ::boost::system::error_code const& errorCode
  , int signum)
{
  DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(handleSignal);

  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_OF_UNKNOWN_THREAD(quitCb_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(signalsRecievedCount_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(signalCallbacks_);

  DVLOG(9)
    << "got signum "
    << std::to_string(signum)
    << " ("
    << getSignalName(signum)
    << ")";

  if(auto it = signalCallbacks_.find(signum); it != signalCallbacks_.end())
  {
    it->second.Run(errorCode, signum);
  } else {
    LOG(WARNING)
    << "Unknown signum "
    << std::to_string(signum)
    << " ("
    << getSignalName(signum)
    << ")";
  }
}

void SignalHandler::handleQuitSignal(
  ::boost::system::error_code const& errorCode
  , int signum)
{
  DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(handleQuitSignal);

  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_OF_UNKNOWN_THREAD(quitCb_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(signalsRecievedCount_);
  DCHECK_MEMBER_OF_UNKNOWN_THREAD(signalCallbacks_);

  signalsRecievedCount_.store(
    signalsRecievedCount_.load() + 1);

  UMA_HISTOGRAM_COUNTS_1000("App.QuitSignalCount"
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

  ::base::rvalue_cast(quitCb_).Run();
}

SignalHandler::~SignalHandler()
{
  DCHECK_RUN_ON(&sequence_checker_);
}

} // namespace signal_handler
} // namespace plugin
