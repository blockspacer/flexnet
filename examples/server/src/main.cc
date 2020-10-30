#include "init_env.hpp"
#include "generated/static_plugins.hpp"
#include "registry/main_loop_registry.hpp"
#include "plugin_manager/plugin_manager.hpp"
#include "plugin_interface/plugin_interface.hpp"

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

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/scoped_sequence_context_var.hpp>
#include <basis/typed_enum.h>
#include <basis/strong_alias.hpp>

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
  = base::Promise<void, base::NoReject>;

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

MUST_USE_RESULT
static VoidPromise startPluginManager() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  SCOPED_UMA_HISTOGRAM_TIMER( /// \note measures up to 10 seconds
    "startPluginManager from " + FROM_HERE.ToString());

  base::FilePath dir_exe;
  if (!base::PathService::Get(base::DIR_EXE, &dir_exe)) {
    NOTREACHED();
  }

  base::CommandLine* cmdLine
    = base::CommandLine::ForCurrentProcess();

  base::FilePath pathToDirWithPlugins
    = cmdLine->HasSwitch(kPluginsDirSwitch)
      ? base::FilePath{cmdLine->GetSwitchValueASCII(
          kPluginsDirSwitch)}
      // default value
      : dir_exe
        .AppendASCII(kRelativePluginsDir);

  base::FilePath pathToPluginsConfFile
  = cmdLine->HasSwitch(kPluginsConfigFileSwitch)
      ? base::FilePath{cmdLine->GetSwitchValueASCII(
          kPluginsConfigFileSwitch)}
      // default value
      : dir_exe
        .AppendASCII(kDefaultPluginsConfigFilesDir)
        .AppendASCII(::plugin::kPluginsConfigFileName);

  std::vector<base::FilePath> pathsToExtraPluginFiles{};

  loadStaticPlugins();

  PluginManager& pluginManager =
    MainLoopRegistry::GetInstance()->registry()
      .ctx<PluginManager>();

  return pluginManager.startup(
    base::rvalue_cast(pathToDirWithPlugins)
    , base::rvalue_cast(pathToPluginsConfFile)
    , base::rvalue_cast(pathsToExtraPluginFiles)
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
    , base::BindOnce(&unloadStaticPlugins)
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
     ::util::Status result =
       appState.processStateChange(
         FROM_HERE
         , AppState::START);
     DCHECK(result.ok());
  }

  ignore_result(
    startPluginManager()
  );

  return
  // async-wait for termination event
  appState.promiseEntryOnce(
    FROM_HERE
    , AppState::TERMINATE)
  .ThenHere(FROM_HERE
    // during teardown we need to be able to perform IO.
    , base::BindOnce(
        &base::ThreadRestrictions::SetIOAllowed
        , true
      )
  )
  .ThenHere(FROM_HERE
    , base::BindOnce(&shutdownPluginManager)
    , base::IsNestedPromise{true}
  )
  .ThenHere(FROM_HERE
    , base::BindOnce(&unsetGlobals)
  );
}

void finishProcessMetrics() NO_EXCEPTION
{
  auto processMetrics
    = base::ProcessMetrics::CreateCurrentProcessMetrics();

  {
    size_t malloc_usage =
        processMetrics->GetMallocUsage();

    VLOG(1)
      << "malloc_usage: "
      << malloc_usage;

    int malloc_usage_mb = static_cast<int>(malloc_usage >> 20);
    base::UmaHistogramMemoryLargeMB(
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
    std::vector<base::SamplingHeapProfiler::Sample> samples =
        base::SamplingHeapProfiler::Get()->GetSamples(
          0 // To retrieve all set to 0.
        );
    if (!samples.empty()) {
      base::ModuleCache module_cache;
      for (const base::SamplingHeapProfiler::Sample& sample : samples) {
        std::vector<base::Frame> frames;
        frames.reserve(sample.stack.size());
        for (const void* frame : sample.stack) {
          uintptr_t address = reinterpret_cast<uintptr_t>(frame);
          const base::ModuleCache::Module* module =
              module_cache.GetModuleForAddress(address);
          frames.emplace_back(address, module);
        }
        size_t count = std::max<size_t>(
            static_cast<size_t>(
                std::llround(static_cast<double>(sample.total) / sample.size)),
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

      for (const auto* module : module_cache.GetModules()) {
        VLOG(1)
          << "module GetDebugBasename: "
          << base::StringPrintf(
                // PRFilePath from <base/files/file_path.h>
                // prints path names portably
                "%" PRFilePath
               , module->GetDebugBasename().value().c_str());
        VLOG(1)
          << "module GetBaseAddress: "
          << base::StringPrintf(
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

namespace base {

// Check lifetime of reference, use memory tool like ASAN
//
// USAGE
//
// {
//   // ERROR: AddressSanitizer: stack-use-after-scope
//   base::MessageLoop::current().task_runner()->PostTask(
//     FROM_HERE
//     , base::bindCheckedOnce(
//         DEBUG_BIND_CHECKS(
//           PTR_CHECKER(&tmpClass)
//         )
//         , &TmpClass::TestMe
//         , base::Unretained(&tmpClass)
//         , base::Passed(FROM_HERE))
//   );
//
//   DVLOG(9)
//     << "TmpClass freed before `runLoop.Run()`"
//     << " i.e. before `PostTask` execution"
//     << " Build with `-fsanitize=address,undefined`"
//     << " to detect `AddressSanitizer: stack-use-after-scope`";
// }
#define PTR_CHECKER(PTR_NAME) \
  base::bindPtrChecker(FROM_HERE, PTR_NAME)

template <typename PtrType>
class PtrChecker
{
 public:
  template <typename U>
  GUARD_METHOD_ON_UNKNOWN_THREAD(PtrChecker)
  explicit
  PtrChecker(
    const base::Location& location
    , U* ptr)
    : ptr_(ptr)
    , location_(location)
  {
    DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(PtrChecker);

    /// \note disallows nullptr
    DCHECK(ptr_)
      << location_.ToString();
    checkForLifetimeIssues();
  }

  // called on callback destruction
  ~PtrChecker()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();
  }

  PtrChecker(PtrChecker<PtrType>&& other)
    : ptr_{base::rvalue_cast(other.ptr_)}
    , location_{CAN_COPY_ON_MOVE("moving const") std::move(other.location_)}
  {}

  PtrChecker& operator=(
    PtrChecker<PtrType>&& other)
  {
    ptr_ = base::rvalue_cast(other.ptr_);
    location_ = CAN_COPY_ON_MOVE("moving const") std::move(other.location_);
    return *this;
  }

  void runCheck()
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    LOG_CALL(DVLOG(99));
    /// \note disallows nullptr
    DCHECK(ptr_)
      << location_.ToString();
    checkForLifetimeIssues();
  }

 private:
  // check that object is alive, use memory tool like ASAN
  inline void checkForLifetimeIssues() const
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    // Works with `-fsanitize=address,undefined
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    if (ptr_)
      reinterpret_cast<const volatile uint8_t*>(ptr_)[0];
#endif
  }

 private:
  PtrType* ptr_ = nullptr;

  const base::Location location_;

  // Object construction can be on any thread
  CREATE_METHOD_GUARD(PtrChecker);

  // All methods except constructor use same thread sequence
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(PtrChecker);
};

template <typename PtrType>
PtrChecker<PtrType> bindPtrChecker(
  const base::Location& location
  , PtrType* ptr)
{
  return PtrChecker<PtrType>{location, ptr};
}

// Check lifetime of reference, use memory tool like ASAN
//
// USAGE
//
// {
//   // ERROR: AddressSanitizer: stack-use-after-scope
//   base::MessageLoop::current().task_runner()->PostTask(
//     FROM_HERE
//     , base::bindCheckedOnce(
//         DEBUG_BIND_CHECKS(
//           REF_CHECKER(tmpClass)
//         )
//         , &TmpClass::TestMe
//         , base::Unretained(&tmpClass)
//         , base::Passed(FROM_HERE))
//   );
//
//   DVLOG(9)
//     << "TmpClass freed before `runLoop.Run()`"
//     << " i.e. before `PostTask` execution"
//     << " Build with `-fsanitize=address,undefined`"
//     << " to detect `AddressSanitizer: stack-use-after-scope`";
// }
#define REF_CHECKER(REF_NAME) \
  base::bindRefChecker(FROM_HERE, REFERENCED(REF_NAME))

// Check lifetime of pointer, use memory tool like ASAN
//
// USAGE
//
// Check lifetime of reference, use memory tool like ASAN
//
// USAGE
//
// {
//   // ERROR: AddressSanitizer: stack-use-after-scope
//   base::MessageLoop::current().task_runner()->PostTask(
//     FROM_HERE
//     , base::bindCheckedOnce(
//         DEBUG_BIND_CHECKS(
//           REF_CHECKER(tmpClass)
//         )
//         , &TmpClass::TestMe
//         , base::Unretained(&tmpClass)
//         , base::Passed(FROM_HERE))
//   );
//
//   DVLOG(9)
//     << "TmpClass freed before `runLoop.Run()`"
//     << " i.e. before `PostTask` execution"
//     << " Build with `-fsanitize=address,undefined`"
//     << " to detect `AddressSanitizer: stack-use-after-scope`";
// }
#define CONST_REF_CHECKER(REF_NAME) \
  base::bindRefChecker(FROM_HERE, CONST_REFERENCED(REF_NAME))

template <typename RefType>
class RefChecker
{
 public:
  template <typename U>
  GUARD_METHOD_ON_UNKNOWN_THREAD(RefChecker)
  explicit
  RefChecker(
    const base::Location& location
    , U& ref)
    : ptr_(&ref)
    , location_(location)
  {
    DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(RefChecker);

    /// \note disallows nullptr
    DCHECK(ptr_)
      << location_.ToString();
    checkForLifetimeIssues();
  }

  // called on callback destruction
  ~RefChecker()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();
  }

  RefChecker(RefChecker<RefType>&& other)
    : ptr_{base::rvalue_cast(other.ptr_)}
    , location_{CAN_COPY_ON_MOVE("moving const") std::move(other.location_)}
  {}

  RefChecker& operator=(
    RefChecker<RefType>&& other)
  {
    ptr_ = base::rvalue_cast(other.ptr_);
    location_ = CAN_COPY_ON_MOVE("moving const") std::move(other.location_);
    return *this;
  }

  void runCheck()
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    LOG_CALL(DVLOG(99));

    /// \note disallows nullptr
    DCHECK(ptr_)
      << location_.ToString();
    checkForLifetimeIssues();
  }

 private:
  // check that object is alive, use memory tool like ASAN
  inline void checkForLifetimeIssues() const
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    // Works with `-fsanitize=address,undefined
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    if (ptr_)
      reinterpret_cast<const volatile uint8_t*>(ptr_)[0];
#endif
  }

 private:
  RefType* ptr_ = nullptr;

  const base::Location location_;

  // Object construction can be on any thread
  CREATE_METHOD_GUARD(RefChecker);

  // All methods except constructor use same thread sequence
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(RefChecker);
};

template <typename RefType>
RefChecker<RefType> bindRefChecker(
  const base::Location& location
  , std::reference_wrapper<RefType> ref)
{
  return RefChecker<RefType>{location, ref.get()};
}

// Check that callback will be invoked only once
//
// USAGE
//
// {
//   base::MessageLoop::current().task_runner()->PostTask(
//     FROM_HERE
//     , base::bindCheckedOnce(
//         DEBUG_BIND_CHECKS(
//           CALLED_ONCE_CHECKER()
//         )
//         , &TmpClass::TestMe
//         , base::Unretained(&tmpClass)
//         , base::Passed(FROM_HERE))
//   );
// }
#define CALLED_ONCE_CHECKER() \
  base::bindCallCountChecker(FROM_HERE, 1)

// Check that callback will be invoked exactly `PARAM` times
//
// USAGE
//
// {
//   base::MessageLoop::current().task_runner()->PostTask(
//     FROM_HERE
//     , base::bindCheckedOnce(
//         DEBUG_BIND_CHECKS(
//           CALL_COUNT_CHECKER(1)
//         )
//         , &TmpClass::TestMe
//         , base::Unretained(&tmpClass)
//         , base::Passed(FROM_HERE))
//   );
// }
#define CALL_COUNT_CHECKER(PARAM) \
  base::bindCallCountChecker(FROM_HERE, PARAM)

/// \note Performs actual check not in `runCheck()`, but in destructor.
template <typename CounterType = size_t>
class CallCountChecker
{
 public:
  static constexpr CounterType kZeroCallCount = 0;

  GUARD_METHOD_ON_UNKNOWN_THREAD(CallCountChecker)
  CallCountChecker(
    const base::Location& location
    , const CounterType& expectedCallCount)
    : callCount_(kZeroCallCount)
    , expectedCallCount_(expectedCallCount)
    , location_(location)
  {
    DCHECK_METHOD_RUN_ON_UNKNOWN_THREAD(CallCountChecker);

    DCHECK_GE(expectedCallCount_, kZeroCallCount)
      << location_.ToString()
      << " Expected call count expected to be >= 0";
  }

  // check call count on callback destruction
  ~CallCountChecker()
  {
    if(is_moved_out_)
    {
      return;
    }

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    LOG_CALL(DVLOG(99));

    DCHECK_EQ(callCount_, expectedCallCount_)
      << location_.ToString()
      << " Call count expected to be: "
      << std::to_string(expectedCallCount_);
  }

  CallCountChecker(CallCountChecker<CounterType>&& other)
    : callCount_{base::rvalue_cast(other.callCount_)}
    , expectedCallCount_{base::rvalue_cast(other.expectedCallCount_)}
    , location_{CAN_COPY_ON_MOVE("moving const") std::move(other.location_)}
  {
    other.is_moved_out_ = true;
  }

  CallCountChecker& operator=(
    CallCountChecker<CounterType>&& other)
  {
    callCount_ = base::rvalue_cast(other.callCount_);
    expectedCallCount_ = base::rvalue_cast(other.expectedCallCount_);
    location_ = CAN_COPY_ON_MOVE("moving const") std::move(other.location_);
    other.is_moved_out_ = true;
    return *this;
  }

  // inc call count on callback invocation
  void runCheck()
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
      << location_.ToString();

    DCHECK(!is_moved_out_);

    LOG_CALL(DVLOG(99));

    DCHECK(callCount_ < std::numeric_limits<CounterType>::max())
      << location_.ToString()
      << " Unable to represent call count in CounterType";

    callCount_++;
  }

 private:
  CounterType callCount_;

  CounterType expectedCallCount_;

  const base::Location location_;

  bool is_moved_out_{false};

  // Object construction can be on any thread
  CREATE_METHOD_GUARD(CallCountChecker);

  // All methods except constructor use same thread sequence
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(CallCountChecker);
};

template <typename CounterType>
CallCountChecker<size_t> bindCallCountChecker(
  const base::Location& location
  , const CounterType& val)
{
  return CallCountChecker<size_t>{
    location
    , base::checked_cast<size_t>(val)
  };
}

// BindChecks<>
//
// This stores all the state passed into bindChecked().
/// \note each type in `typename... BoundArgs` must have `runCheck()` method.
template <typename... BoundArgs>
struct BindChecks final
{
  template <typename... ForwardBoundArgs>
  explicit BindChecks(ForwardBoundArgs&&... bound_args)
    : bound_checks_(std::forward<ForwardBoundArgs>(bound_args)...)
  {}

  template <typename... ForwardBoundArgs>
  BindChecks(BindChecks<ForwardBoundArgs...>&& other)
    : bound_checks_{base::rvalue_cast(other.bound_checks_)}
  {}

  template <typename... ForwardBoundArgs>
  BindChecks& operator=(
    BindChecks<ForwardBoundArgs...>&& other)
  {
    bound_checks_ = base::rvalue_cast(other.bound_checks_);
    return *this;
  }

  template <typename... ForwardBoundArgs>
  BindChecks(const BindChecks<ForwardBoundArgs...>& other) = delete;

  template <typename... ForwardBoundArgs>
  BindChecks& operator=(
    const BindChecks<ForwardBoundArgs...>& other) = delete;

  ~BindChecks() = default;

  std::tuple<BoundArgs...> bound_checks_;

 private:
  DISALLOW_COPY_AND_ASSIGN(BindChecks);
};

/// \note prefer `DEBUG_BIND_CHECKS` for performance reasons
#define BIND_CHECKS(...) \
  base::buildBindChecks(__VA_ARGS__)

#if DCHECK_IS_ON()
#define DEBUG_BIND_CHECKS(...) \
  BIND_CHECKS(__VA_ARGS__)
#else
#define DEBUG_BIND_CHECKS(...)
#endif

template <typename... ForwardBoundArgs>
BindChecks<ForwardBoundArgs...> buildBindChecks(
  ForwardBoundArgs&&... bound_args)
{
  return BindChecks<ForwardBoundArgs...>{
    std::forward<ForwardBoundArgs>(bound_args)...
  };
}

namespace internal {

// CheckedBindState<>
//
// This stores all the state passed into Bind().
template <typename CheckerType, typename Functor, typename... BoundArgs>
struct CheckedBindState final : BindStateBase {
  using IsCancellable = std::integral_constant<
      bool,
      CallbackCancellationTraits<Functor,
                                 std::tuple<BoundArgs...>>::is_cancellable>;

  template <
    typename Checker
    , typename ForwardFunctor
    , typename... ForwardBoundArgs
  >
  static CheckedBindState* Create(BindStateBase::InvokeFuncStorage invoke_func,
                           Checker&& checker,
                           ForwardFunctor&& functor,
                           ForwardBoundArgs&&... bound_args) {
    // Ban ref counted receivers that were not yet fully constructed to avoid
    // a common pattern of racy situation.
    BanUnconstructedRefCountedReceiver<ForwardFunctor>(bound_args...);

    LOG_CALL(DVLOG(99));

    // IsCancellable is std::false_type if
    // CallbackCancellationTraits<>::IsCancelled returns always false.
    // Otherwise, it's std::true_type.
    return new CheckedBindState(IsCancellable{}, invoke_func,
                         base::rvalue_cast(checker),
                         std::forward<ForwardFunctor>(functor),
                         std::forward<ForwardBoundArgs>(bound_args)...);
  }

  CheckerType checker_;
  Functor functor_;
  std::tuple<BoundArgs...> bound_args_;

 private:
  template <
    typename Checker
    , typename ForwardFunctor
    , typename... ForwardBoundArgs
  >
  explicit CheckedBindState(std::true_type,
                     BindStateBase::InvokeFuncStorage invoke_func,
                     Checker&& checker,
                     ForwardFunctor&& functor,
                     ForwardBoundArgs&&... bound_args)
      : BindStateBase(invoke_func,
                      &Destroy,
                      &QueryCancellationTraits<CheckedBindState>),
        checker_{base::rvalue_cast(checker)},
        functor_(std::forward<ForwardFunctor>(functor)),
        bound_args_(std::forward<ForwardBoundArgs>(bound_args)...)
  {
    LOG_CALL(DVLOG(99));
    DCHECK(!IsNull(functor_));
  }

  template <
    typename Checker
    , typename ForwardFunctor
    , typename... ForwardBoundArgs
  >
  explicit CheckedBindState(std::false_type,
                     BindStateBase::InvokeFuncStorage invoke_func,
                     Checker&& checker,
                     ForwardFunctor&& functor,
                     ForwardBoundArgs&&... bound_args)
      : BindStateBase(invoke_func, &Destroy),
        checker_{base::rvalue_cast(checker)},
        functor_(std::forward<ForwardFunctor>(functor)),
        bound_args_(std::forward<ForwardBoundArgs>(bound_args)...)
  {
    LOG_CALL(DVLOG(99));
    DCHECK(!IsNull(functor_));
  }

  ~CheckedBindState()
  {
    LOG_CALL(DVLOG(99));
  }

  static void Destroy(const BindStateBase* self)
  {
    LOG_CALL(DVLOG(99));
    delete static_cast<const CheckedBindState*>(self);
  }
};

// Used to implement MakeCheckedBindStateType.
template <
  typename CheckerType,
  bool is_method,
  typename Functor,
  typename... BoundArgs
>
struct MakeCheckedBindStateTypeImpl;

template <
  typename CheckerType,
  typename Functor,
  typename... BoundArgs
>
struct MakeCheckedBindStateTypeImpl<CheckerType, false, Functor, BoundArgs...> {
  static_assert(!HasRefCountedTypeAsRawPtr<std::decay_t<BoundArgs>...>::value,
                "A parameter is a refcounted type and needs scoped_refptr.");
  using Type = CheckedBindState<CheckerType, std::decay_t<Functor>, std::decay_t<BoundArgs>...>;
};

template <
  typename CheckerType,
  typename Functor
>
struct MakeCheckedBindStateTypeImpl<CheckerType, true, Functor> {
  using Type = CheckedBindState<CheckerType, std::decay_t<Functor>>;
};

template <
  typename CheckerType,
  typename Functor,
  typename Receiver,
  typename... BoundArgs
>
struct MakeCheckedBindStateTypeImpl<CheckerType, true, Functor, Receiver, BoundArgs...> {
 private:
  using DecayedReceiver = std::decay_t<Receiver>;

  static_assert(!std::is_array<std::remove_reference_t<Receiver>>::value,
                "First bound argument to a method cannot be an array.");
  static_assert(
      !std::is_pointer<DecayedReceiver>::value ||
          IsRefCountedType<std::remove_pointer_t<DecayedReceiver>>::value,
      "Receivers may not be raw pointers. If using a raw pointer here is safe"
      " and has no lifetime concerns, use base::Unretained() and document why"
      " it's safe.");
  static_assert(!HasRefCountedTypeAsRawPtr<std::decay_t<BoundArgs>...>::value,
                "A parameter is a refcounted type and needs scoped_refptr.");

 public:
  using Type = CheckedBindState<
      CheckerType,
      std::decay_t<Functor>,
      std::conditional_t<std::is_pointer<DecayedReceiver>::value,
                         scoped_refptr<std::remove_pointer_t<DecayedReceiver>>,
                         DecayedReceiver>,
      std::decay_t<BoundArgs>...>;
};

template <typename CheckerType, typename Functor, typename... BoundArgs>
using MakeCheckedBindStateType =
    typename MakeCheckedBindStateTypeImpl<
                                   CheckerType,
                                   MakeFunctorTraits<Functor>::is_method,
                                   Functor,
                                   BoundArgs...>::Type;

// InvokerWithChecks<>
//
// See description at the top of the file.
template <typename StorageType, typename UnboundRunType>
struct InvokerWithChecks;

template <typename StorageType, typename R, typename... UnboundArgs>
struct InvokerWithChecks<StorageType, R(UnboundArgs...)>
{
  using UnboundRunType = R(UnboundArgs...);
  using WrappedInvoker = internal::Invoker<StorageType, UnboundRunType>;

  static R RunOnce(BindStateBase* base,
                   PassingType<UnboundArgs>... unbound_args)
  {
    LOG_CALL(DVLOG(99));

    StorageType* storage = static_cast<StorageType*>(base);

    static constexpr size_t num_bound_checks =
        std::tuple_size<decltype(storage->checker_.bound_checks_)>::value;

    runBindCheckers(
      storage->checker_.bound_checks_
      , std::make_index_sequence<num_bound_checks>());

    return WrappedInvoker::RunOnce(
      base
      , std::forward<UnboundArgs>(unbound_args)...);
  }

  static R Run(BindStateBase* base, PassingType<UnboundArgs>... unbound_args) {

    StorageType* storage = static_cast<StorageType*>(base);

    static constexpr size_t num_bound_checks =
        std::tuple_size<decltype(storage->checker_.bound_checks_)>::value;

    runBindCheckers(
      storage->checker_.bound_checks_
      , std::make_index_sequence<num_bound_checks>());

    return WrappedInvoker::Run(
      base
      , std::forward<UnboundArgs>(unbound_args)...);
  }

 private:
  template<typename... Args>
  static inline void runBindChecker(Args&&... passedArgs)
  {
    /// \note fold expression requires C++17
    ((void)(passedArgs.runCheck()), ...);
  }

  template <typename BoundChecksTuple, size_t... indices>
  static inline void runBindCheckers(
    BoundChecksTuple&& bound
    , std::index_sequence<indices...>)
  {
    using DecayedArgsTuple = std::decay_t<BoundChecksTuple>;

    runBindChecker(std::get<indices>(std::forward<DecayedArgsTuple>(bound))...);
  }
};

} // namespace internal

// Bind as OnceCallback.
template <typename CheckerType, typename Functor, typename... Args>
inline OnceCallback<MakeUnboundRunType<Functor, Args...>>
bindCheckedOnce(CheckerType&& checker
  , Functor&& functor
  , Args&&... args)
{
  static_assert(!internal::IsOnceCallback<std::decay_t<Functor>>() ||
                    (std::is_rvalue_reference<Functor&&>() &&
                     !std::is_const<std::remove_reference_t<Functor>>()),
                "BindOnce requires non-const rvalue for OnceCallback binding."
                " I.e.: base::BindOnce(std::move(callback)).");

  // This block checks if each |args| matches to the corresponding params of the
  // target function. This check does not affect the behavior of Bind, but its
  // error message should be more readable.
  using Helper = internal::BindTypeHelper<Functor, Args...>;
  using FunctorTraits = typename Helper::FunctorTraits;
  using BoundArgsList = typename Helper::BoundArgsList;
  using UnwrappedArgsList =
      internal::MakeUnwrappedTypeList<true, FunctorTraits::is_method,
                                      Args&&...>;
  using BoundParamsList = typename Helper::BoundParamsList;
  static_assert(internal::AssertBindArgsValidity<
                    std::make_index_sequence<Helper::num_bounds>, BoundArgsList,
                    UnwrappedArgsList, BoundParamsList>::ok,
                "The bound args need to be convertible to the target params.");

  using CheckedBindState = internal::MakeCheckedBindStateType<
      CheckerType, Functor, Args...
    >;
  using UnboundRunType = MakeUnboundRunType<Functor, Args...>;
  using CheckedInvoker = internal::InvokerWithChecks<CheckedBindState, UnboundRunType>;
  using CallbackType = OnceCallback<UnboundRunType>;

  // Store the invoke func into PolymorphicInvoke before casting it to
  // InvokeFuncStorage, so that we can ensure its type matches to
  // PolymorphicInvoke, to which CallbackType will cast back.
  using PolymorphicInvoke = typename CallbackType::PolymorphicInvoke;
  PolymorphicInvoke invoke_func = &CheckedInvoker::RunOnce;

  LOG_CALL(DVLOG(99));

  using InvokeFuncStorage = internal::BindStateBase::InvokeFuncStorage;
  CallbackType result = CallbackType(CheckedBindState::Create(
      reinterpret_cast<InvokeFuncStorage>(invoke_func),
      base::rvalue_cast(checker),
      std::forward<Functor>(functor), std::forward<Args>(args)...));

  LOG_CALL(DVLOG(99));

  return result;
}

// Special cases for binding to a base::Callback without extra bound arguments.
template <typename Signature>
OnceCallback<Signature> bindCheckedOnce(OnceCallback<Signature> closure) {
  return closure;
}

} // namespace basis

class TmpClass
{
 public:
  TmpClass(){}

  void TestMe(const base::Location& location)
  {
    LOG_CALL(DVLOG(99))
      << " a "
      << a
      << " b "
      << b
      << " location.ToString() "
      << location.ToString();
  }

  int a = 6;
  std::string b = "hi!";
};

int main(int argc, char* argv[])
{
  // stores basic requirements, like thread pool, logging, etc.
  basis::ScopedBaseEnvironment base_env;

  // init common application systems,
  // initialization order matters!
  {
    base::Optional<int> exit_code = initEnv(
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
  base::RunLoop runLoop;

  /// \note Task will be executed
  /// when `runLoop.Run()` called.
  base::PostPromise(FROM_HERE,
    base::MessageLoop::current().task_runner().get()
    , base::BindOnce(&runServerAndPromiseQuit)
    , base::IsNestedPromise{true}
  )
  .ThenHere(FROM_HERE
    , base::BindOnce(&finishProcessMetrics)
  )
  // Stop `base::RunLoop` when `base::Promise` resolved.
  .ThenHere(FROM_HERE
    , runLoop.QuitClosure()
  );

  //{
  //  DVLOG(9)
  //    << "CallCountChecker1 created";
  //  base::CallCountChecker cc1{FROM_HERE, 1};
  //  DVLOG(9)
  //    << "CallCountChecker2 created";
  //  base::CallCountChecker cc2{base::rvalue_cast(cc1)};
  //  cc2.runCheck();
  //  DVLOG(9)
  //    << "CallCountChecker3 created";
  //}

  DVLOG(9)
    << "test1 created";
  base::OnceCallback<int(const std::string&, int)> test1
    = base::bindCheckedOnce(
        DEBUG_BIND_CHECKS(
          REF_CHECKER(runLoop)
          , PTR_CHECKER(&runLoop)
          , CALL_COUNT_CHECKER(1)
        )
        , [
        ](
          const std::string&, int
        ){
          return 4;
        }
      );
  int a = 4;
  DVLOG(9)
    << "test1.Run start";
  DCHECK(base::rvalue_cast(test1).Run("hi", 2) == 4);
  DVLOG(9)
    << "test1.Run done";

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<base::SequencedTaskRunner> task_runner =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  TmpClass tmpClass;
  {
    base::MessageLoop::current().task_runner()->PostTask(
      FROM_HERE
      , base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            REF_CHECKER(tmpClass)
            , PTR_CHECKER(&tmpClass)
            , CALL_COUNT_CHECKER(1)
          )
          , &TmpClass::TestMe
          , base::Unretained(&tmpClass)
          , base::Passed(FROM_HERE))
    );

    task_runner->PostTask(
      FROM_HERE
      , base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            REF_CHECKER(tmpClass)
            , PTR_CHECKER(&tmpClass)
            , CALL_COUNT_CHECKER(1)
          )
          , &TmpClass::TestMe
          , base::Unretained(&tmpClass)
          , base::Passed(FROM_HERE))
    );
  }

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
