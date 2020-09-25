#include "init_env.hpp"
#include "generated/static_plugins.hpp"
#include "server_run_loop_state.hpp"
#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "registry/main_loop_registry.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>

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

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
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
#include <basis/scoped_sequence_context_var.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

using namespace flexnet;
using namespace backend;

using VoidPromise
  = base::Promise<void, base::NoReject>;

using PluginManager
  = plugin::PluginManager<
      ::plugin::PluginInterface
    >;

static const char kPluginsConfigFilesDir[]
  = "resources/configuration_files";

static const char kPluginsDirName[]
  = "plugins";

MUST_USE_RESULT
static VoidPromise startPluginManager() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  base::FilePath dir_exe;
  if (!base::PathService::Get(base::DIR_EXE, &dir_exe)) {
    NOTREACHED();
  }

  base::FilePath pathToDirWithPlugins
  = dir_exe
      .AppendASCII(kPluginsDirName);

  base::FilePath pathToPluginsConfFile
  = dir_exe
      .AppendASCII(kPluginsConfigFilesDir)
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
/// \note Remove in order reverse to construction.
static void unsetGlobals() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(base::RunLoop::IsRunningOnCurrentThread());

  MainLoopRegistry::GetInstance()->registry()
    .unset<AppState>();

  MainLoopRegistry::GetInstance()->registry()
    .unset<PluginManager>();
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
    , base::BindOnce(&shutdownPluginManager)
    , base::IsNestedPromise{true}
  )
  .ThenHere(FROM_HERE
    , base::BindOnce(
        base::BindOnce(&unsetGlobals)
      )
  );
}

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
  // Stop `base::RunLoop` when `base::Promise` resolved.
  .ThenHere(FROM_HERE
    , runLoop.QuitClosure()
  );

  /// \note blocks util `runLoop.QuitClosure()` called
  runLoop.Run();

  DVLOG(9)
    << "Main run loop finished";

  return
    EXIT_SUCCESS;
}
