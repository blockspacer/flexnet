#include "init_env.hpp"
#include "example_server.hpp"
#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"

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

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

#include <Corrade/Containers/Pointer.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/Utility/Configuration.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Directory.h>

/** Import static plugins

If you link static plugins to your executable, they can't automatically
register themselves at startup to be known to
@ref Corrade::PluginManager::Manager "PluginManager::Manager", and you need to
load them explicitly by calling @ref CORRADE_PLUGIN_IMPORT() at the beginning
of the @cpp main() @ce function. You can also wrap these macro calls in another
function (which will then be compiled into a dynamic library or the main
executable) and use the @ref CORRADE_AUTOMATIC_INITIALIZER() macro for an
automatic call
*/
static void loadStaticPlugins()
{
  GENERATE_CORRADE_PLUGIN_IMPORT
}

/** Eject a previously imported static plugins
(plugin name same as defined with CORRADE_PLUGIN_REGISTER())

Deregisters a plugin previously registered using @ref CORRADE_PLUGIN_IMPORT().

@attention This macro should be called outside of any namespace. See the
    @ref CORRADE_RESOURCE_INITIALIZE() macro for more information.

Functions called by this macro don't do any dynamic allocation or other
operations that could fail, so it's safe to call it even in restricted phases
of application exection. It's also safe to call this macro more than once.
*/
static void unloadStaticPlugins()
{
  GENERATE_CORRADE_PLUGIN_EJECT
}

using namespace flexnet;
using namespace backend;

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

  loadStaticPlugins();

  ServerRunLoopContext serverRunLoopContext;

  serverRunLoopContext.runLoop();

  unloadStaticPlugins();

  LOG(INFO)
    << "server is quitting";

  return
    EXIT_SUCCESS;
}
