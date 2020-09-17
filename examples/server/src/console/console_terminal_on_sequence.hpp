#pragma once

#include "console/console_feature_list.hpp"
#include "console/console_input_updater.hpp"

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

#include <basis/scoped_sequence_context_var.hpp>
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

// Creates periodic console input updater (`ConsoleInputUpdater`)
// in sequence-local-storage of `periodicConsoleTaskRunner`
/// \note Manipulation of sequence-local-storage is asyncronous,
/// so you must wait for construction/deletion or use `base::Promise`
class ConsoleTerminalOnSequence
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  ConsoleTerminalOnSequence(
    scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner);

  /// \note API is asyncronous, so you must check
  /// if `destructScopedSequenceCtxVar` finished
  // Wait for deletion otherwise you can get `use-after-free` error
  // on resources bound to periodic callback.
  VoidPromise promiseDeletion();

  ~ConsoleTerminalOnSequence();

  // constructs object in sequence-local-storage
  template <class... Args>
  VoidPromise promiseEmplaceAndStart(
    const base::Location& from_here
    , const std::string& debug_name
    , Args&&... args) NO_EXCEPTION
  {
    LOG_CALL(DVLOG(99));

    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicConsoleTaskRunner_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(consoleInputUpdaterOnSequence_));

    return consoleInputUpdaterOnSequence_
    .emplace_async(from_here
        , debug_name
        , std::forward<Args>(args)...
    )
    // start `periodicTaskExecutor` after object was constructed
    // in sequence-local-storage
    .ThenOn(periodicConsoleTaskRunner_
      , FROM_HERE
      , base::BindOnce(
        []
        (
          scoped_refptr<base::SequencedTaskRunner> periodicRunner
          , ConsoleInputUpdater* consoleUpdater
        ){
          LOG_CALL(DVLOG(99));

          if(!base::FeatureList::IsEnabled(kFeatureConsoleTerminal))
          {
            DVLOG(99)
              << "console terminal not enabled";
            return;
          }

          /// \todo make period configurable
          consoleUpdater->periodicTaskExecutor().startPeriodicTimer(
            base::TimeDelta::FromMilliseconds(100));

          DCHECK(periodicRunner->RunsTasksInCurrentSequence());
        }
        , periodicConsoleTaskRunner_
      )
    );
  }

 private:
  SET_WEAK_POINTERS(ConsoleTerminalOnSequence);

  // Task sequence used to update text input from console terminal.
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(periodicConsoleTaskRunner_));

  /// \note Will free stored variable on scope exit.
  // Use UNIQUE type to store in sequence-local-context
  /// \note initialized, used and destroyed
  /// on `TaskRunner` sequence-local-context
  basis::ScopedSequenceCtxVar<ConsoleInputUpdater> consoleInputUpdaterOnSequence_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(consoleInputUpdaterOnSequence_));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ConsoleTerminalOnSequence);
};

} // namespace backend
