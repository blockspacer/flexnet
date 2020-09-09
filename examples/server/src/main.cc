#include "init_env.hpp"
#include "example_server.hpp"
#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/util/lock_with_check.hpp>
#include <flexnet/util/periodic_validate_until.hpp>

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

  /// \todo custom config manager
  /// ConfigManager configManager;

  ExampleServer exampleServer;

  exampleServer.runLoop();

  LOG(INFO)
    << "server is quitting";

  return
    EXIT_SUCCESS;
}
