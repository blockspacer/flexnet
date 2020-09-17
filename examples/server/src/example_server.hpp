#pragma once

#include "console/console_terminal_on_sequence.hpp"
#include "net/network_entity_updater_on_sequence.hpp"
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

 private:
  // Loads configurations,
  // required before first run.
  void prepareBeforeRunLoop() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  // Periodically reads line from console terminal
  void updateConsoleTerminal() NO_EXCEPTION
    RUN_ON(periodicConsoleTaskRunner_.get());

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
  // The io_context is required for all I/O
  boost::asio::io_context ioc_
    SET_STORAGE_THREAD_GUARD(guard_ioc_);

  const EndpointType tcpEndpoint_
    GUARDED_BY(sequence_checker_);

  /// \todo SSL support
  // ::boost::asio::ssl::context ctx_
  //   {::boost::asio::ssl::context::tlsv12}
  //   SET_STORAGE_THREAD_GUARD(guard_mainLoopRunner_);

  ECS::AsioRegistry asioRegistry_
    SET_STORAGE_THREAD_GUARD(guard_asioRegistry_);

  TcpEntityAllocator tcpEntityAllocator_;
    SET_STORAGE_THREAD_GUARD(guard_tcpEntityAllocator_);

  // Listens for tcp connections.
  ws::Listener listener_
    GUARDED_BY(sequence_checker_);

  // Main loop that performs scheduled tasks.
  base::RunLoop run_loop_
    GUARDED_BY(sequence_checker_);

  // Captures SIGINT and SIGTERM to perform a clean shutdown
  /// \note `boost::asio::signal_set` will not handle signals if ioc stopped
  boost::asio::signal_set signals_set_
    GUARDED_BY(sequence_checker_);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    SET_STORAGE_THREAD_GUARD(guard_mainLoopRunner_);

  /// \todo custom thread message pump
  base::Thread asio_thread_1
    GUARDED_BY(sequence_checker_);

  /// \todo custom thread message pump
  base::Thread asio_thread_2
    GUARDED_BY(sequence_checker_);

  /// \todo custom thread message pump
  base::Thread asio_thread_3
    GUARDED_BY(sequence_checker_);

  /// \todo custom thread message pump
  base::Thread asio_thread_4
    GUARDED_BY(sequence_checker_);

  // Task sequence used to update asio registry.
  scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner_
    SET_STORAGE_THREAD_GUARD(guard_periodicAsioTaskRunner_);

  // Task sequence used to update text input from console terminal.
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner_
    SET_STORAGE_THREAD_GUARD(guard_periodicConsoleTaskRunner_);

  // On scope exit will schedule destruction (from sequence-local-context),
  // so use `base::Optional` to control scope i.e. control lifetime.
  base::Optional<ConsoleTerminalOnSequence> consoleTerminal_;

  // On scope exit will schedule destruction (from sequence-local-context),
  // so use `base::Optional` to control scope i.e. control lifetime.
  base::Optional<NetworkEntityUpdaterOnSequence> networkEntityUpdater_;

  // Used to free network resources.
  basis::PeriodicValidateUntil periodicValidateUntil_
    // Can be used only by one thread at some moment of time
    // (but different threads can use it overall)
    SET_THREAD_COLLISION_GUARD(guard_periodicValidateUntil_);

  /// \todo
  //TcpEntityAllocator tcpEntityAllocator_;

  // |stream_| moved in |onDetected|
  std::atomic<bool> is_terminating_
    // assumed to be thread-safe
    SET_CUSTOM_THREAD_GUARD(guard_is_terminating_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

} // namespace backend
