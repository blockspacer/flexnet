#pragma once

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
#include <basis/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

using namespace flexnet;
using namespace backend;

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
    const std::string& ip_addr = "127.0.0.1"
    , const unsigned short port_num = 8085);

  ~ExampleServer();

  void runLoop() NO_EXCEPTION;

 private:
  // Loads configurations,
  // required before first run.
  void prepareBeforeRunLoop() NO_EXCEPTION
    RUN_ON(&sequence_checker_);

  NOT_THREAD_SAFE_FUNCTION()
  void updateAsioRegistry() NO_EXCEPTION;

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptors() NO_EXCEPTION;

  MUST_USE_RETURN_VALUE
  VoidPromise configureAndRunAcceptor() NO_EXCEPTION;

  // Call during server termination.
  /// \note make sure connections recieved `stop` message
  /// and acceptors stopped
  /// i.e. do not allocate new connections
  MUST_USE_RETURN_VALUE
  VoidPromise promiseNetworkResourcesFreed() NO_EXCEPTION;

  void stopIOContext() NO_EXCEPTION;

  void hangleQuitSignal();

 private:
  // The io_context is required for all I/O
  boost::asio::io_context ioc_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(ioc_);

  const EndpointType tcpEndpoint_
    GUARDED_BY(sequence_checker_);

  /// \todo SSL support
  // ::boost::asio::ssl::context ctx_
  //   {::boost::asio::ssl::context::tlsv12}
  //   // It safe to read value from any thread because its storage
  //   // expected to be not modified (if properly initialized)
  //   SET_CUSTOM_THREAD_GUARD(mainLoopRunner_);

  ECS::AsioRegistry asioRegistry_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(asioRegistry_);

  ws::Listener listener_
    GUARDED_BY(sequence_checker_);

  base::RunLoop run_loop_
    GUARDED_BY(sequence_checker_);

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  /// \note will not handle signals if ioc stopped
  boost::asio::signal_set signals_set_
    GUARDED_BY(sequence_checker_);

  // `mainLoopRunner_.RunsTasksInCurrentSequence()` expected to be same as
  // `sequence_checker_.CalledOnValidSequence()`
  scoped_refptr<base::SingleThreadTaskRunner>
    mainLoopRunner_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(mainLoopRunner_);

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

  scoped_refptr<base::SequencedTaskRunner>
    periodicAsioTaskRunner_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);

  basis::PeriodicValidateUntil periodicValidateUntil_{};

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

} // namespace backend
