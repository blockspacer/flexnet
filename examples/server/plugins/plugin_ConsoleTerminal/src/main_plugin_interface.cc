#include "main_plugin_interface.hpp" // IWYU pragma: associated
#include "main_plugin_constants.hpp"

#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

namespace plugin {
namespace console_terminal {

MainPluginInterface::MainPluginInterface(
  AbstractManager &manager
  , const std::string &pluginName)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , ::plugin::PluginInterface{manager, pluginName}
  , title_(metadata()->data().value("title"))
  , author_(metadata()->data().value("author"))
  , description_(metadata()->data().value("description"))
  , mainLoopRunner_(base::MessageLoop::current()->task_runner())
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

MainPluginInterface::~MainPluginInterface()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);
}

std::string MainPluginInterface::title() const
{
  DCHECK_RUN_ON(&sequence_checker_);

  return title_;
}

std::string MainPluginInterface::author() const
{
  DCHECK_RUN_ON(&sequence_checker_);

  return author_;
}

std::string MainPluginInterface::description() const
{
  DCHECK_RUN_ON(&sequence_checker_);

  return description_;
}

MainPluginInterface::VoidPromise
  MainPluginInterface::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginInterface::load()");

  DLOG(INFO)
    << "loading plugin with title = "
    << title_
    << " and description = "
    << description_.substr(0, 1000)
    << "...";

  mainPluginLogic_.emplace(this);

  return
      ::base::PostPromise(FROM_HERE
        , mainLoopRunner_.get()
        , ::base::bindCheckedOnce(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(mainLoopRunner_.get())
            )
            , [ ]() {
              LOG_CALL(DVLOG(99))
                  << kPluginName
                  << " starting...";
            })
        )
      .ThenHere(FROM_HERE
        , ::base::bindCheckedOnce(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(&mainPluginLogic_.value())
            )
            , &MainPluginLogic::load
            , ::base::Unretained(&mainPluginLogic_.value()))
        , ::base::IsNestedPromise{true}
      )
      .ThenHere(FROM_HERE
        , ::base::BindOnce(
          [
          ](
          ){
            LOG_CALL(DVLOG(99))
                << kPluginName
                << " started...";
          })
      );
}

MainPluginInterface::VoidPromise MainPluginInterface::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginInterface::unload()");

  DLOG(INFO)
    << "unloading plugin with title = "
    << title_
    << " and description = "
    << description_.substr(0, 1000)
    << "...";

  DCHECK(mainPluginLogic_);

  return
      ::base::PostPromise(FROM_HERE
        , mainLoopRunner_.get()
        , ::base::bindCheckedOnce(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(mainLoopRunner_.get())
            )
            , []() {
                LOG_CALL(DVLOG(99))
                  << kPluginName
                  << " terminating...";
              })
      )
      .ThenHere(FROM_HERE
        , ::base::bindCheckedOnce(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(&mainPluginLogic_.value())
            )
            , &MainPluginLogic::unload
            , ::base::Unretained(&mainPluginLogic_.value()))
        , ::base::IsNestedPromise{true}
      )
      .ThenHere(FROM_HERE
        , ::base::BindOnce(
            &base::Optional<MainPluginLogic>::reset
            , ::base::Unretained(&mainPluginLogic_)
          )
      )
      .ThenHere(FROM_HERE
        , ::base::BindOnce(
          [
          ](
          ){
            LOG_CALL(DVLOG(99))
              << kPluginName
              << " terminated...";
          })
      );
}

int MainPluginInterface::consoleInputFreqMillisec() const NO_EXCEPTION
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG_CALL(DVLOG(99));

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = metadata()->configuration();

  int consoleInputFreqMillisec
    = kDefaultConsoleInputFreqMillisec;

  if(configuration.hasValue(kConfConsoleInputFreqMillisec))
  {
    ::base::StringToInt(
      configuration.value(kConfConsoleInputFreqMillisec)
      , &consoleInputFreqMillisec);
  }

  return consoleInputFreqMillisec;
}

} // namespace console_terminal
} // namespace plugin
