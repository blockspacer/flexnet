#include "init_env.hpp"
#include "generated/static_plugins.hpp"
#include "registry/main_loop_registry.hpp"
#include "plugin_manager/plugin_manager.hpp"
#include "plugin_interface/plugin_interface.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/callback_list.h>
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
#include <base/command_line.h>
#include <base/numerics/safe_conversions.h>
#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/sampling_heap_profiler/sampling_heap_profiler.h>
#include <base/sampling_heap_profiler/poisson_allocation_sampler.h>
#include <base/sampling_heap_profiler/module_cache.h>
#include <base/process/process_metrics.h>
#include <base/allocator/partition_allocator/page_allocator.h>
#include <base/allocator/allocator_shim.h>
#include <base/allocator/buildflags.h>
#include <base/allocator/partition_allocator/partition_alloc.h>
#include <base/profiler/frame.h>
#include <base/trace_event/malloc_dump_provider.h>
#include <base/trace_event/memory_dump_provider.h>
#include <base/trace_event/memory_dump_scheduler.h>
#include <base/trace_event/memory_infra_background_whitelist.h>
#include <base/trace_event/process_memory_dump.h>
#include <base/trace_event/trace_event.h>
#include <base/allocator/allocator_check.h>
#include <base/strings/stringprintf.h>
#include <base/strings/string_number_conversions.h>
#include <base/compiler_specific.h>
#include <base/template_util.h>
#include <base/threading/thread_collision_warner.h>
#include <base/strings/substitute.h>
#include <base/strings/utf_string_conversions.h>

#include <basis/status/app_error_space.hpp>
#include <basis/core/typed_enum.hpp>
#include <basis/fail_point/fail_point.hpp>
#include <basis/plug_point/plug_point.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/dummy_checker.hpp>
#include <basis/bind/ptr_checker.hpp>
#include <basis/bind/ref_checker.hpp>
#include <basis/bind/call_count_checker.hpp>
#include <basis/bind/delay_time_checker.hpp>
#include <basis/bind/exec_time_checker.hpp>
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/strong_types/strong_alias.hpp>
#include <basis/strong_types/strong_bool.hpp>
#include <basis/strong_types/strong_string.hpp>
#include <basis/strong_types/strong_int.hpp>
#include <basis/strong_types/strong_size_units.hpp>
#include <basis/strong_types/strong_time_units.hpp>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>
#include <basis/bind/callable_hook.hpp>
#include <basis/status/with_details.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <chrono>
#include <string>

using namespace flexnet;
using namespace backend;

using VoidPromise
  = ::base::Promise<void, ::base::NoReject>;

using PluginManager
  = plugin::PluginManager<
      ::plugin::PluginInterface
    >;

static const char kDefaultPluginsConfigFilesDir[]
  = "resources/configuration_files";

// example: --plugins_conf=$PWD/conf/plugins.conf
static const char kPluginsConfigFileSwitch[]
  = "plugins_conf";

static const char kRelativePluginsDir[]
  = "plugins";

// example: --plugins_dir=$PWD/plugins
static const char kPluginsDirSwitch[]
  = "plugins_dir";

//#define TODO_TESTS 1

#if TODO_TESTS

static ::basis::StatusOr<int> ParseInt(const std::string& str)
{
  using namespace ::app_error_space;

  std::size_t index = 0;
  int result = 0;
  if(::base::StringToInt(str, &result)){
    // successful conversion
    return {FROM_HERE, result};
  }

  // something in the string stopped the conversion, at index
  RETURN_ERROR(ERR_INVALID_PARAM).without_logging()
    << "Invalid input string:"
    << str;
}

static ::basis::Status GetContents(const std::string& path, std::string* data)
{
  using namespace ::app_error_space;

  if(path.empty())
    RETURN_ERROR(ERR_INVALID_PARAM)
        << "Invalid path.";

  *data = path; // for test purposes only

  RETURN_OK();
}

static ::basis::StatusOr<int> ReadNumber(const std::string& path) {
  std::string data;
  RETURN_IF_ERROR(GetContents(path, &data));

  ASSIGN_OR_RETURN(int number, ParseInt(data));
  return {FROM_HERE, number};
}

static ::basis::Status testErrInternal()
{
  ::basis::Status status =
    MAKE_ERROR()
      << "testErrInternal_text";
  return status;
}

static ::basis::Status testErrInternal2()
{
  RETURN_ERROR(/*ERR_INVALID_PARAM*/)
      << "testErrInternal2_text";
}

static ::basis::Status testErrInternal3()
{
  int port = -1;

  RETURN_ERR_IF_FALSE(port >= 0)
      << "Port ID must be non-negative. Attempted to get port " << port;

  NOTREACHED();

  //RETURN_ERROR(/*ERR_INVALID_PARAM*/);
  ::basis::Status status =
    MAKE_ERROR()
      << "testErrInternal3_text";
  return status;
}

static ::basis::Status testOk()
{
  RETURN_OK();
}

static ::basis::Status testOk2()
{
  return ::basis::OkStatus(FROM_HERE);
}

static ::basis::StatusOr<std::string> or_testErrInternal()
{
  ::basis::StatusOr<std::string> status =
    MAKE_ERROR()
      << "or_testErrInternal_text";
  return status;
}

static ::basis::StatusOr<std::string> or_testErrInternal2()
{
  RETURN_ERROR(/*ERR_INVALID_PARAM*/)
      << "Unsupported or_testErrInternal2.";
}

static ::basis::StatusOr<std::string> or_testErr()
{
  using namespace ::app_error_space;

  ::basis::StatusOr<std::string> status =
    MAKE_ERROR(ERR_PERMISSION_DENIED)
      << "testCustomErr";

  return status;
}

static ::basis::StatusOr<std::string> or_testOk()
{
  return {FROM_HERE, "or_testOk!!"};
}

static ::basis::StatusOr<std::string> or_testErrInternal3()
{
  int port = -1;

  RETURN_ERR_IF_FALSE(port >= 0)
      << "Port ID must be non-negative. Attempted to get port " << port;

  NOTREACHED();

  //RETURN_ERROR(/*ERR_INVALID_PARAM*/);
  ::basis::Status status =
    MAKE_ERROR()
      << "or_testErrInternal3_text";
  return status;
}

static ::basis::StatusOr<std::string> or_testOk2()
{
  return {FROM_HERE, "or_testOk2!!"};
}
#endif

MUST_USE_RESULT
static VoidPromise startPluginManager() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  SCOPED_UMA_HISTOGRAM_TIMER( /// \note measures up to 10 seconds
    "startPluginManager from " + FROM_HERE.ToString());

  ::base::FilePath dir_exe;
  if (!base::PathService::Get(base::DIR_EXE, &dir_exe)) {
    NOTREACHED();
  }

  ::base::CommandLine* cmdLine
    = ::base::CommandLine::ForCurrentProcess();
  DCHECK_PTR(cmdLine);

  ::base::FilePath pathToDirWithPlugins
    = cmdLine->HasSwitch(kPluginsDirSwitch)
      ? ::base::FilePath{cmdLine->GetSwitchValueASCII(
          kPluginsDirSwitch)}
      // default value
      : dir_exe
        .AppendASCII(kRelativePluginsDir);

  ::base::FilePath pathToPluginsConfFile
  = cmdLine->HasSwitch(kPluginsConfigFileSwitch)
      ? ::base::FilePath{cmdLine->GetSwitchValueASCII(
          kPluginsConfigFileSwitch)}
      // default value
      : dir_exe
        .AppendASCII(kDefaultPluginsConfigFilesDir)
        .AppendASCII(::plugin::kPluginsConfigFileName);

  std::vector<::base::FilePath> pathsToExtraPluginFiles{};

  loadStaticPlugins();

  PluginManager& pluginManager =
    MainLoopRegistry::GetInstance()->registry()
      .ctx<PluginManager>();

  return pluginManager.startup(
    ::base::rvalue_cast(pathToDirWithPlugins)
    , ::base::rvalue_cast(pathToPluginsConfFile)
    , ::base::rvalue_cast(pathsToExtraPluginFiles)
  );
}

MUST_USE_RESULT
static VoidPromise shutdownPluginManager() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  PluginManager& pluginManager =
    MainLoopRegistry::GetInstance()->registry()
      .ctx<PluginManager>();

  return pluginManager.shutdown()
  .ThenHere(FROM_HERE
    /// \note call after `pluginManager.shutdown()` finished
    , ::base::BindOnce(&unloadStaticPlugins)
  );
}

// Add objects into global storage.
static void setGlobals() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  AppState& appState =
    MainLoopRegistry::GetInstance()->registry()
      .set<AppState>(AppState::UNINITIALIZED);

  PluginManager& pluginManager =
    MainLoopRegistry::GetInstance()->registry()
      .set<PluginManager>();
}

// Remove objects from global storage.
/// \note `unset` in order reverse to `set`.
static void unsetGlobals() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  MainLoopRegistry::GetInstance()->registry()
    .unset<PluginManager>();

  MainLoopRegistry::GetInstance()->registry()
    .unset<AppState>();
}

MUST_USE_RESULT
static VoidPromise runServerAndPromiseQuit() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  setGlobals();

  AppState& appState =
    MainLoopRegistry::GetInstance()->registry()
      .ctx<AppState>();

  {
     ::basis::Status result =
       appState.processStateChange(
         FROM_HERE
         , AppState::START);
     DCHECK(result.ok());
  }

  ignore_result(
    startPluginManager()
  );

#if TODO_TESTS
  {
    ::basis::Status resErrInternal
      = testErrInternal();
    ::basis::Status resErrInternal2
      = testErrInternal2();
    ::basis::Status resErrInternal3
      = testErrInternal3();
    ::basis::Status resOk
      = testOk();
    ::basis::Status resOk2
      = testOk2();
    DVLOG(99)
      << " resErrInternal "
      << resErrInternal;
    DVLOG(99)
      << " resErrInternal2 "
      << resErrInternal2;
    DVLOG(99)
      << " resErrInternal3 "
      << resErrInternal3;
    DVLOG(99)
      << " resOk "
      << resOk;
    DVLOG(99)
      << " resOk2 "
      << resOk2;
  }

  {
    ::basis::StatusOr<std::string> or_resErrInternal
      = or_testErrInternal();
    ::basis::StatusOr<std::string> or_resErrInternal2
      = or_testErrInternal2();
    ::basis::StatusOr<std::string> or_resErrInternal3
      = or_testErrInternal3();
    ::basis::StatusOr<std::string> or_testErr1
      = or_testErr();
    ::basis::StatusOr<std::string> or_resOk
      = or_testOk();
    ::basis::StatusOr<std::string> or_resOk2
      = or_testOk2();
    DVLOG(99)
      << " or_resErrInternal "
      << or_resErrInternal;
    DVLOG(99)
      << " or_resErrInternal2 "
      << or_resErrInternal2;
    DVLOG(99)
      << " or_resErrInternal3 "
      << or_resErrInternal3;
    DVLOG(99)
      << " or_testErr1 "
      << or_testErr1;
    DVLOG(99)
      << " or_resOk "
      << or_resOk;
    DVLOG(99)
      << " or_resOk2 "
      << or_resOk2;
  }

  DVLOG(99)
    << " ReadNumber 327: "
    << ReadNumber("327"); // OK

  DVLOG(99)
    << " ReadNumber abc: "
    << ReadNumber("abc"); // ERR

  DVLOG(99)
    << " ReadNumber xyz: ";
  ::basis::StatusOr<int> readNumberStatusOr = ReadNumber("xyz");
  // `!status.ok()` required by APPEND_ERROR
  DCHECK(!readNumberStatusOr.ok());
  ::basis::Status readNumberStatus = readNumberStatusOr.status();
  DVLOG(99)
    << " Performing APPEND_ERROR ";
  readNumberStatus = APPEND_ERROR(readNumberStatus)//.with_log_stack_trace()
      << " Custom error appended.";
  DVLOG(99)
    << " Performing APPEND_ERROR ";
  readNumberStatus = APPEND_ERROR(readNumberStatus).without_logging()
      << " Another error appended.";
  DVLOG(99)
    << " Performing APPEND_STATUS_IF_ERROR ";
  APPEND_STATUS_IF_ERROR(readNumberStatus, ParseInt("foo").status());
  DVLOG(99)
    << " Result xyz: "
    << readNumberStatus; // ERR
  LOG_IF_ERROR(readNumberStatus);

  exit(0);
#endif

  return
  // async-wait for termination event
  appState.promiseEntryOnce(
    FROM_HERE
    , AppState::TERMINATE)
  .ThenHere(FROM_HERE
    // during teardown we need to be able to perform IO.
    , ::base::BindOnce(
        &base::ThreadRestrictions::SetIOAllowed
        , true
      )
  )
  .ThenHere(FROM_HERE
    , ::base::BindOnce(&shutdownPluginManager)
    , ::base::IsNestedPromise{true}
  )
  .ThenHere(FROM_HERE
    , ::base::BindOnce(&unsetGlobals)
  );
}

/// \note metrics expected to be reported after termination of all plugins,
/// so we do not create separate plugin for `finishProcessMetrics`
/// (so we able to profile plugin manager heap)
void finishProcessMetrics() NO_EXCEPTION
{
  auto processMetrics
    = ::base::ProcessMetrics::CreateCurrentProcessMetrics();

  {
    size_t malloc_usage =
        processMetrics->GetMallocUsage();

    VLOG(1)
      << "malloc_usage: "
      << malloc_usage;

    int malloc_usage_mb = static_cast<int>(malloc_usage >> 20);
    ::base::UmaHistogramMemoryLargeMB(
      "App.Memory.HeapProfiler.Malloc"
      , malloc_usage_mb);
  }

  {
    size_t cpu_usage
      = processMetrics->GetPlatformIndependentCPUUsage();

    VLOG(1)
      << "cpu_usage: "
      << cpu_usage;

    // We scale up to the equivalent of 64 CPU cores fully loaded.
    // More than this does not really matter,
    // as we are already in a terrible place.
    const int kHistogramMin = 1;
    const int kHistogramMax = 6400;
    const int kHistogramBucketCount = 50;

    UMA_HISTOGRAM_CUSTOM_COUNTS(
        "App.AverageCPUUsage", cpu_usage,
        kHistogramMin, kHistogramMax, kHistogramBucketCount);
  }

  {
    // sampling profiling of native memory heap.
    // Aggregates the heap allocations and records samples
    // using GetSamples method.
    std::vector<::base::SamplingHeapProfiler::Sample> samples =
        ::base::SamplingHeapProfiler::Get()->GetSamples(
          0 // To retrieve all set to 0.
        );
    if (!samples.empty()) {
      ::base::ModuleCache module_cache;
      for (const ::base::SamplingHeapProfiler::Sample& sample : samples) {
        std::vector<::base::Frame> frames;
        frames.reserve(sample.stack.size());
        for (const void* frame : sample.stack)
        {
          DCHECK_PTR(frame);
          uintptr_t address = reinterpret_cast<uintptr_t>(frame);
          const ::base::ModuleCache::Module* module =
              module_cache.GetModuleForAddress(address);
          DCHECK_PTR(module);
          frames.emplace_back(address, module);
        }
        size_t count = std::max<size_t>(
          static_cast<size_t>(
            std::llround(
              static_cast<double>(sample.total) / static_cast<double>(sample.size))),
          1);
        VLOG(1)
          << "SamplingHeapProfiler"
             " sample.total: "
          << sample.total;
        VLOG(1)
          << "SamplingHeapProfiler"
             " sample.total / sample.size: "
          << count;
      }

      std::map<size_t, size_t> h_buckets;
      std::map<size_t, size_t> h_sums;
      for (auto& sample : samples) {
        h_buckets[sample.size] += sample.total;
      }
      for (auto& it : h_buckets) {
        h_sums[it.first] += it.second;
      }
      for (auto& it : h_sums) {
        VLOG(1)
          << "SamplingHeapProfiler"
             " h_sums: "
          << it.second;
      }

      for (const auto* module : module_cache.GetModules())
      {
        DCHECK_PTR(module);
        VLOG(1)
          << "module GetDebugBasename: "
          << ::base::StringPrintf(
                // PRFilePath from <base/files/file_path.h>
                // prints path names portably
                "%" PRFilePath
               , module->GetDebugBasename().value().c_str());
        VLOG(1)
          << "module GetBaseAddress: "
          << ::base::StringPrintf(
                // PRIxPTR from <base/basictypes.h>
                // i.e. <inttypes.h>
                "0x%" PRIxPTR
               , module->GetBaseAddress());
        VLOG(1)
          << "module module->GetSize(): "
          << module->GetSize();
      }
    }
  }
}

int main(int argc, char* argv[])
{
  // stores basic requirements, like thread pool, logging, etc.
  ::basis::ScopedBaseEnvironment base_env;

  // init common application systems,
  // initialization order matters!
  {
    ::base::Optional<int> exit_code = initEnv(
      argc
      , argv
      , base_env);
    if(exit_code.has_value()) {
      LOG(WARNING)
        << "exited during environment creation";
      return exit_code.value();
    }
  }

  // Main loop that performs scheduled tasks.
  ::base::RunLoop runLoop;

  /// \note Task will be executed
  /// when `runLoop.Run()` called.
  ::base::PostPromise(FROM_HERE,
    ::base::MessageLoop::current().task_runner().get()
    , ::base::BindOnce(&runServerAndPromiseQuit)
    , ::base::IsNestedPromise{true}
  )
  .ThenHere(FROM_HERE
    , ::base::BindOnce(&finishProcessMetrics)
  )
  // Stop `base::RunLoop` when `base::Promise` resolved.
  .ThenHere(FROM_HERE
    , runLoop.QuitClosure()
  );

  {
    /// \note blocks util `runLoop.QuitClosure()` called
    runLoop.Run();

    /// \note Do NOT use `base::MessageLoop::current().task_runner()`
    /// after `runLoop.Run` finished (otherwise posted tasks
    /// will be NOT executed i.e. scheduled forever).
    DCHECK(!base::RunLoop::IsRunningOnCurrentThread());
  }

  DVLOG(9)
    << "Main run loop finished";

  return
    EXIT_SUCCESS;
}
