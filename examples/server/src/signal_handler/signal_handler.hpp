#pragma once

#include "console_terminal/console_terminal_plugin.hpp"
#include "console_terminal/console_terminal_on_sequence.hpp"
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

class SignalHandler
{
 public:
  SignalHandler(
    ::boost::asio::io_context& ioc
    , base::OnceClosure&& quitCb)
    RUN_ON(&sequence_checker_);

  ~SignalHandler()
    RUN_ON(&sequence_checker_);

 private:
  void handleQuitSignal(::boost::system::error_code const&, int)
    RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(FUNC_GUARD(handleQuitSignal));

 private:
  // Capture SIGINT and SIGTERM to perform a clean shutdown
  ::boost::asio::signal_set signals_set_
    GUARDED_BY(sequence_checker_);

  base::OnceClosure quitCb_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(quitCb_));

  std::atomic<size_t> signalsRecievedCount_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(signalsRecievedCount_));

  /// \note can be called from any thread
  CREATE_CUSTOM_THREAD_GUARD(FUNC_GUARD(handleQuitSignal));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(SignalHandler);
};

} // namespace backend
