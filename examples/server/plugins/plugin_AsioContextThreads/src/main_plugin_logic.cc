#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

namespace plugin {
namespace asio_context_threads {

MainPluginLogic::MainPluginLogic(
  const MainPluginInterface* pluginInterface)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , mainLoopRunner_{
      base::MessageLoop::current()->task_runner()}
  , mainLoopRegistry_(
      ::backend::MainLoopRegistry::GetInstance())
  , ioc_(REFERENCED(
      ::backend::MainLoopRegistry::GetInstance()->registry()
        .set<::boost::asio::io_context>()))
  , asioRegistry_{
      REFERENCED(mainLoopRegistry_->registry()
        .set<ECS::AsioRegistry>(REFERENCED(*ioc_)))}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  int confAsioThreads
    = kDefaultAsioThreads;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = pluginInterface->metadata()->configuration();

  if(configuration.hasValue(kConfAsioThreads))
  {
    base::StringToInt(
      configuration.value(kConfAsioThreads)
      , &confAsioThreads);
  }

  asioThreadsManager_.startThreads(
    /// \note Crash if out of range.
    base::checked_cast<size_t>(confAsioThreads)
    , REFERENCED(*ioc_)
  );
}

MainPluginLogic::~MainPluginLogic()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  asioThreadsManager_.stopThreads();
}

MainPluginLogic::VoidPromise
  MainPluginLogic::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::load()");

  return
    VoidPromise::CreateResolved(FROM_HERE);
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK(mainLoopRunner_);

  return
    base::PostPromise(FROM_HERE
      , UNOWNED_LIFETIME(mainLoopRunner_.get())
      , base::BindOnce(
        [
        ](
        ){
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

} // namespace asio_context_threads
} // namespace plugin
