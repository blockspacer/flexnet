#include "init_env.hpp" // IWYU pragma: associated

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

#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

/// \todo make configurable
const char DEFAULT_EVENT_CATEGORIES[]
  = "-sequence_manager"
    ",-thread_pool"
    ",-base"
    ",-toplevel"
    ",profiler"
    ",user_timing"
    ",ui"
    ",browser"
    ",latency"
    ",latencyInfo"
    ",loading"
    ",skia"
    ",task_scheduler"
    ",native"
    ",benchmark"
    ",ipc"
    ",mojom"
    ",media"
    ",disabled-by-default-lifecycles"
    ",disabled-by-default-renderer.scheduler"
    ",disabled-by-default-v8.gc"
    ",disabled-by-default-blink_gc"
    ",disabled-by-default-system_stats"
    ",disabled-by-default-network"
    ",disabled-by-default-cpu_profiler"
    ",disabled-by-default-memory-infra";

/// \todo make configurable
static constexpr ::base::FilePath::CharType kIcuDataFileName[]
  = FILE_PATH_LITERAL(R"raw(./resources/icu/optimal/icudt64l.dat)raw");

/// \todo make configurable
static constexpr ::base::FilePath::CharType kTraceReportFileName[]
  = FILE_PATH_LITERAL(R"raw(trace_report.json)raw");

MUST_USE_RETURN_VALUE
base::Optional<int> initEnv(
  int argc
  , char* argv[]
  , ::basis::ScopedBaseEnvironment& base_env
  )
{
  ::base::FilePath dir_exe;
  if (!base::PathService::Get(base::DIR_EXE, &dir_exe)) {
    NOTREACHED();
    return EXIT_FAILURE;
  }

  // ScopedBaseEnvironment
  {
    const bool envCreated
      = base_env.init(
          argc
          , argv
          , false // AutoStartTracer
          , DEFAULT_EVENT_CATEGORIES // tracingCategories
          , dir_exe // current working dir
          , kIcuDataFileName
          , kTraceReportFileName
          /// \note number of threads in global thread pool
          , 11 // threadsNum
          );
    if(!envCreated) {
      LOG(ERROR)
        << "Unable to create base environment";
      return
        EXIT_FAILURE;
    }
  }

  /// \todo use |SequenceLocalContext|
#if 0
  // Stores vector of arbitrary typed objects,
  // each object can be found by its type (using entt::type_info).
  ECS::GlobalContext* globals
    = ECS::GlobalContext::GetInstance();
  DCHECK(globals);

  // |GlobalContext| is not thread-safe,
  // so modify it only from one sequence
  globals->unlockModification();

  // main ECS registry
  ECS::SimulationRegistry& enttManager
    = globals->set_once<ECS::SimulationRegistry>(FROM_HERE,
        "Ctx_SimulationRegistry");

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<::base::SequencedTaskRunner> entt_task_runner =
    ::base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      ::base::TaskTraits{
        ::base::TaskPriority::BEST_EFFORT
        , ::base::MayBlock()
        , ::base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  // ECS registry is not thread-safe
  // i.e. manipulate sessions in single sequence.
  enttManager.set_task_runner(entt_task_runner);
#endif // 0

  return ::base::nullopt;
}

} // namespace backend
