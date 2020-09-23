#pragma once

#include "network_entity_updater.hpp"

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

// Creates periodic updater for `network-ECS`
// in sequence-local-storage of `periodicAsioTaskRunner`
/// \note Manipulation of sequence-local-storage is asyncronous,
/// so you must wait for construction/deletion or use `base::Promise`
class NetworkEntityUpdaterOnSequence
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  NetworkEntityUpdaterOnSequence(
    scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner);

  /// \note API is asyncronous, so you must check
  /// if `destructScopedCrossSequenceCtxVar` finished
  // Wait for deletion otherwise you can get `use-after-free` error
  // on resources bound to periodic callback.
  VoidPromise promiseDeletion();

  ~NetworkEntityUpdaterOnSequence();

  // constructs object in sequence-local-storage
  template <class... Args>
  VoidPromise promiseEmplaceAndStart(
    const base::Location& from_here
    , const std::string& debug_name
    , Args&&... args) NO_EXCEPTION
  {
    LOG_CALL(DVLOG(99));

    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicAsioTaskRunner_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(networkEntityUpdater_));

    return networkEntityUpdater_
    .emplace_async(from_here
        , debug_name
        , std::forward<Args>(args)...
    )
    // start `periodicTaskExecutor` after object was constructed
    // in sequence-local-storage
    .ThenOn(periodicAsioTaskRunner_
      , FROM_HERE
      , base::BindOnce(
        []
        (
          scoped_refptr<base::SequencedTaskRunner> periodicRunner
          , NetworkEntityUpdater* asioUpdater
        ){
          LOG_CALL(DVLOG(99));

          /// \todo make period configurable
          asioUpdater->periodicTaskExecutor().startPeriodicTimer(
            base::TimeDelta::FromMilliseconds(10));

          DCHECK(periodicRunner->RunsTasksInCurrentSequence());
        }
        , periodicAsioTaskRunner_
      )
    );
  }

 private:
  SET_WEAK_POINTERS(NetworkEntityUpdaterOnSequence);

  // Task sequence used to update `network-ECS`
  scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(periodicAsioTaskRunner_));

  /// \note Will free stored variable on scope exit.
  // Use UNIQUE type to store in sequence-local-context
  /// \note initialized, used and destroyed
  /// on `TaskRunner` sequence-local-context
  basis::ScopedCrossSequenceCtxVar<NetworkEntityUpdater> networkEntityUpdater_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(networkEntityUpdater_));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(NetworkEntityUpdaterOnSequence);
};

} // namespace backend
