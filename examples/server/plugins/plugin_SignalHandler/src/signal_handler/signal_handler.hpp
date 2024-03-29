#pragma once

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
#include <basis/checks_and_guard_annotations.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/safe_registry.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/strong_types/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>
#include <map>
#include <string>

namespace plugin {
namespace signal_handler {

using SignalHandlerCb
  = ::base::RepeatingCallback<
      void(::boost::system::error_code const& errorCode, int signum)
    >;

using SignalHandlerMap = std::map<int, SignalHandlerCb> ;

class SignalHandler
{
 public:
  SignalHandler(
    ::boost::asio::io_context& ioc
    , ::base::OnceClosure&& quitCb)
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

  ~SignalHandler()
    PUBLIC_METHOD_RUN_ON(&sequence_checker_);

 private:
  void handleSignal(::boost::system::error_code const&, int);

  void handleQuitSignal(::boost::system::error_code const&, int);

 private:
  // Capture SIGINT and SIGTERM to perform a clean shutdown
  ::boost::asio::signal_set signals_set_
    GUARDED_BY(sequence_checker_);

  ::base::OnceClosure quitCb_;

  std::atomic<size_t> signalsRecievedCount_;

  SignalHandlerMap signalCallbacks_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(SignalHandler);
};

} // namespace signal_handler
} // namespace plugin
