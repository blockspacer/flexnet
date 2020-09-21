#pragma once

#include "signal_handler/signal_handler.hpp"
#include "console/console_terminal_plugin.hpp"
#include "net/network_entity_plugin.hpp"
#include "console/console_terminal_on_sequence.hpp"
#include "net/network_entity_updater_on_sequence.hpp"
#include "net/asio_threads_manager.hpp"
#include "tcp_entity_allocator.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/optional.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
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
#include <basis/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

using namespace flexnet;

class ConsoleTerminalOnSequence;

struct ServerStartOptions
{
  const std::string ip_addr = "127.0.0.1";
  const unsigned short port_num = 8085;
};

class ExampleServer
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  using EndpointType
    = ws::Listener::EndpointType;

 public:
  ExampleServer(
    ServerStartOptions startOptions = ServerStartOptions());

  ~ExampleServer();

  void runLoop() NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

  // Posts task that will start application termination
  // i.e. call is asynchronous returns immediately.
  void doQuit()
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

  void handleConsoleInput(const std::string& line);

  // Append promise to chain as nested promise.
  void setPromiseBeforeStop(
    /*shared lifetime*/ VoidPromise promise) NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_)
  {
    DCHECK_RUN_ON(&sequence_checker_);

    promiseBeforeStop_ = promise;
  }

  MUST_USE_RESULT
  VoidPromise promiseBeforeStop() NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_)
  {
    DCHECK_RUN_ON(&sequence_checker_);

    return promiseBeforeStop_;
  }

  // Append promise to chain as nested promise.
  void setPromiseBeforeStart(
    /*shared lifetime*/ VoidPromise promise) NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_)
  {
    DCHECK_RUN_ON(&sequence_checker_);

    promiseBeforeStart_ = promise;
  }

  MUST_USE_RESULT
  VoidPromise promiseBeforeStart() NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_)
  {
    DCHECK_RUN_ON(&sequence_checker_);

    return promiseBeforeStart_;
  }

  SET_WEAK_SELF(ExampleServer)

 private:
  void startThreadsManager() NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(&sequence_checker_);

  void startAcceptors() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  // Stops creation of new connections.
  /// \note Existing connections may be in `constructing` state
  /// and they can be fully created at arbitrary (not known beforehand) time
  /// due to asynchronous design of task scheduler.
  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptors() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  // Allow creation of new connections.
  MUST_USE_RETURN_VALUE
  VoidPromise configureAndRunAcceptor() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  // Call during server termination to make sure
  // that connections recieved `stop` message
  // and all acceptors were stopped.
  /// \note Creates task runner to perform periodic check
  /// if `registry.empty()`.
  /// We assume that empty ECS registry means that network resources were freed.
  MUST_USE_RETURN_VALUE
  VoidPromise promiseNetworkResourcesFreed() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  // send async-close for each connection
  // (used on app termination)
  void closeNetworkResources() NO_EXCEPTION
    RUN_ON(periodicValidateUntil_.taskRunner().get());

  // Posts task to strand associated with registry
  // that performs periodic check if `registry.empty()`.
  // `resolveCallback` will be executed if `registry.empty()`.
  // We assume that empty ECS registry means that network resources were freed.
  void validateAndFreeNetworkResources(
    base::RepeatingClosure resolveCallback) NO_EXCEPTION
    RUN_ON(periodicValidateUntil_.taskRunner().get());

  // Stop the `io_context`. This will cause `io_context.run()`
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  void stopIOContext() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

 private:
  SET_WEAK_POINTERS(ExampleServer);

  /// \todo replace with AppStatePromise
  // Must be resolved before `RunLoop.Run` called.
  base::ManualPromiseResolver<void, base::NoReject>
    beforeStartResolver_
    GUARDED_BY(sequence_checker_);

  /// \todo replace with AppStatePromise
  VoidPromise promiseBeforeStart_
    GUARDED_BY(sequence_checker_);

  /// \todo replace with AppStatePromise
  // Must be resolved before `RunLoop.QuitClosure` called.
  base::ManualPromiseResolver<void, base::NoReject>
    beforeStopResolver_
    GUARDED_BY(sequence_checker_);

  /// \todo replace with AppStatePromise
  VoidPromise promiseBeforeStop_
    GUARDED_BY(sequence_checker_);

  // The io_context is required for all I/O
  boost::asio::io_context ioc_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(ioc_));

  const EndpointType tcpEndpoint_
    GUARDED_BY(sequence_checker_);

  /// \todo SSL support
  // ::boost::asio::ssl::context ctx_
  //   {::boost::asio::ssl::context::tlsv12}
  //   SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(mainLoopRunner_));

  ECS::AsioRegistry asioRegistry_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(asioRegistry_));

  TcpEntityAllocator tcpEntityAllocator_;
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(tcpEntityAllocator_));

  // Listens for tcp connections.
  ws::Listener listener_
    GUARDED_BY(sequence_checker_);

  // Main loop that performs scheduled tasks.
  base::RunLoop run_loop_
    GUARDED_BY(sequence_checker_);

  // Captures SIGINT and SIGTERM to perform a clean shutdown
  /// \note `boost::asio::signal_set` will not handle signals if ioc stopped
  SignalHandler signalHandler_
    GUARDED_BY(sequence_checker_);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(mainLoopRunner_));

  // Used to free network resources.
  basis::PeriodicValidateUntil periodicValidateUntil_
    SET_THREAD_COLLISION_GUARD(MEMBER_GUARD(periodicValidateUntil_));

  /// \todo custom plugin manager
  /// PluginManager pluginManager;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  /// \todo use plugin loader
  /// ConfigManager configManager;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  /// \todo use plugin loader
  ConsoleTerminalPlugin consoleTerminalPlugin;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  /// \todo use plugin loader
  NetworkEntityPlugin networkEntityPlugin;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  /// \todo use plugin loader
  AsioThreadsManager asioThreadsManager_;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

} // namespace backend
