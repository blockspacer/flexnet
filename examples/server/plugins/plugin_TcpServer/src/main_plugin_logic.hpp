#pragma once

#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"
#include "state/app_state.hpp"
#include "tcp_entity_allocator.hpp"

#include <base/logging.h>
#include <base/cpu.h>
#include <base/macros.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/strings/string_util.h>
#include <base/trace_event/trace_event.h>
#include <base/strings/string_number_conversions.h>
#include <base/numerics/safe_conversions.h>
#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/log/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/status/statusor.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/strong_types/strong_alias.hpp>
#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/tags.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/task/periodic_task_executor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

#include <thread>
#include <memory>
#include <chrono>

namespace plugin {
namespace tcp_server {

class MainPluginInterface;

// Performs plugin logic based on
// provided plugin interface (configuration)
class MainPluginLogic
{
 public:
  using VoidPromise
    = ::base::Promise<void, ::base::NoReject>;

  using StatusPromise
    = ::base::Promise<::basis::Status, ::base::NoReject>;

  using EndpointType
    = ::boost::asio::ip::tcp::endpoint;

  using ErrorCode
    = ::boost::beast::error_code;

  using SocketType
    = ::boost::asio::ip::tcp::socket;

  using IoContext
    = ::boost::asio::io_context;

  using AcceptorType
    = ::boost::asio::ip::tcp::acceptor;

  using ExecutorType
    // usually same as `::boost::asio::io_context::executor_type`
    = ::boost::asio::executor;

  using StrandType
    = ::boost::asio::strand<ExecutorType>;

 public:
  SET_WEAK_SELF(MainPluginLogic)

  MainPluginLogic(
    const MainPluginInterface* pluginInterface)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  ~MainPluginLogic()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise load()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  VoidPromise unload()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

 private:
  void startAcceptors() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  // Allow creation of new connections.
  MUST_USE_RETURN_VALUE
  VoidPromise configureAndRunAcceptor() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  // Call during server termination to make sure
  // that connections recieved `stop` message
  // and all acceptors were stopped.
  /// \note Creates task runner to perform periodic check
  /// if `registry.empty()`.
  /// We assume that empty ECS registry means that network resources were freed.
  MUST_USE_RETURN_VALUE
  VoidPromise promiseNetworkResourcesFreed() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

  // send async-close for each connection
  // (used on app termination)
  void closeNetworkResources() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(periodicValidateUntil_.taskRunner().get());

  // Posts task to strand associated with registry
  // that performs periodic check if `registry.empty()`.
  // `resolveCallback` will be executed if `registry.empty()`.
  // We assume that empty ECS registry means that network resources were freed.
  void validateAndFreeNetworkResources(
    ::base::RepeatingClosure resolveCallback) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(periodicValidateUntil_.taskRunner().get());

  // Stop the `io_context`. This will cause `io_context.run()`
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  void stopIOContext() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&sequence_checker_);

 private:
  SET_WEAK_POINTERS(MainPluginLogic);

  // Used to free network resources.
  ::basis::PeriodicValidateUntil periodicValidateUntil_
    GUARD_MEMBER_DISALLOW_THREAD_COLLISION(periodicValidateUntil_);

  const MainPluginInterface* pluginInterface_
    GUARDED_BY(sequence_checker_);

  SCOPED_UNOWNED_PTR_CHECKER(pluginInterface_);

  ::backend::MainLoopRegistry* mainLoopRegistry_
    GUARDED_BY(sequence_checker_);

  SCOPED_UNOWNED_PTR_CHECKER(mainLoopRegistry_);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<::base::SingleThreadTaskRunner> mainLoopRunner_;

  ::boost::asio::io_context& ioc_;

  SCOPED_UNOWNED_REF_CHECKER(ioc_);

  const EndpointType tcpEndpoint_
    GUARDED_BY(sequence_checker_);

  ECS::SafeRegistry& registry_;

  SCOPED_UNOWNED_REF_CHECKER(registry_);

  ::backend::TcpEntityAllocator tcpEntityAllocator_;
    GUARD_MEMBER_DISALLOW_THREAD_COLLISION(tcpEntityAllocator_);

  // Listens for tcp connections.
  flexnet::ws::Listener listener_
    GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(MainPluginLogic);
};

} // namespace tcp_server
} // namespace plugin
