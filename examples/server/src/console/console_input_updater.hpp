#pragma once

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

// Creates `periodicTaskExecutor_` that will
// run `update()` on `periodicConsoleTaskRunner`
class ConsoleInputUpdater
{
 public:
  using HandleConsoleInputCb
    = base::RepeatingCallback<void(const std::string&)>;

  ConsoleInputUpdater(
    scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner
    , HandleConsoleInputCb consoleInputCb);

  ~ConsoleInputUpdater();

  // Periodically reads line from console terminal
  void update() NO_EXCEPTION
    RUN_ON_LOCKS_EXCLUDED(periodicConsoleTaskRunner_.get());

  MUST_USE_RETURN_VALUE
  basis::PeriodicTaskExecutor& periodicTaskExecutor() NO_EXCEPTION
    RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(FUNC_GUARD(periodicTaskExecutor));

  SET_WEAK_SELF(ConsoleInputUpdater)

 private:
  SET_WEAK_POINTERS(ConsoleInputUpdater);

  // Task sequence used to update text input from console terminal.
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(periodicConsoleTaskRunner_));

  HandleConsoleInputCb consoleInputCb_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(consoleInputCb_));

  /// \note will stop periodic timer on scope exit
  basis::PeriodicTaskExecutor periodicTaskExecutor_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(periodicTaskExecutor_));

  /// \note can be called from any thread
  CREATE_CUSTOM_THREAD_GUARD(FUNC_GUARD(periodicTaskExecutor));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ConsoleInputUpdater);
};

} // namespace backend
