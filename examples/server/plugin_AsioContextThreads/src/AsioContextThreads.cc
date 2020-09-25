#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"
#include "net/asio_threads_manager.hpp"

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

#include <basis/scoped_sequence_context_var.hpp>
#include <basis/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>

namespace plugin {
namespace asio_context_threads {

static constexpr int kDefaultAsioThreads
  = 4;

static constexpr char kConfAsioThreads[]
  = "asioThreads";

class AsioContextThreadsPlugin
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

 public:
  AsioContextThreadsPlugin(
    const Corrade::PluginManager::PluginMetadata* metadata);

  ~AsioContextThreadsPlugin();

 private:
  SET_WEAK_POINTERS(AsioContextThreadsPlugin);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(mainLoopRunner_));

  util::UnownedRef<::boost::asio::io_context> ioc_;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  /// \todo use plugin loader
  ::backend::AsioThreadsManager asioThreadsManager_;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(AsioContextThreadsPlugin);
};

AsioContextThreadsPlugin::AsioContextThreadsPlugin(
  const Corrade::PluginManager::PluginMetadata* metadata)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
    , mainLoopRunner_{
        base::MessageLoop::current()->task_runner()}
    , ioc_(REFERENCED(
        ::backend::MainLoopRegistry::GetInstance()->registry()
          .set<::boost::asio::io_context>()))
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  int confAsioThreads
    = kDefaultAsioThreads;

  if(metadata->configuration().hasValue(kConfAsioThreads))
  {
    base::StringToInt(
      metadata->configuration().value(kConfAsioThreads)
      , &confAsioThreads);
  }

  asioThreadsManager_.startThreads(
    /// \note Crash if out of range.
    base::checked_cast<size_t>(confAsioThreads)
    , REFERENCED(*ioc_)
  );
}

AsioContextThreadsPlugin::~AsioContextThreadsPlugin()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  asioThreadsManager_.stopThreads();
}

class AsioContextThreads
  final
  : public ::plugin::PluginInterface {
 public:
  explicit AsioContextThreads(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
  {
    LOG_CALL(DVLOG(99));

    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~AsioContextThreads()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  std::string title() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("title");
  }

  std::string author() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("author");
  }

  std::string description() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("description");
  }

  VoidPromise load() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::AsioContextThreads::load()");

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
             LOG_CALL(DVLOG(99))
              << " AsioContextThreads starting...";
          })
      );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::AsioContextThreads::unload()");

    DLOG(INFO)
      << "unloaded plugin with title = "
      << title()
      << " and description = "
      << description().substr(0, 100)
      << "...";

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
             LOG_CALL(DVLOG(99))
              << " AsioContextThreads terminating...";

             {
               VLOG(1)
                 << "stopping io context";

               ::boost::asio::io_context& ioc =
                 ::backend::MainLoopRegistry::GetInstance()->registry()
                   .ctx<::boost::asio::io_context>();

               // Stop the `io_context`. This will cause `io_context.run()`
               // to return immediately, eventually destroying the
               // io_context and any remaining handlers in it.
               ioc.stop(); // io_context::stop is thread-safe
               DCHECK(ioc.stopped()); // io_context::stopped is thread-safe
             }
          })
      );
  }

private:
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  AsioContextThreadsPlugin asioContextThreadsPlugin_{metadata()};

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(AsioContextThreads);
};

} // namespace asio_context_threads
} // namespace plugin

REGISTER_PLUGIN(/*name*/ AsioContextThreads
    , /*className*/ plugin::asio_context_threads::AsioContextThreads
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
