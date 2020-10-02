#include "main_plugin_interface.hpp" // IWYU pragma: associated
#include "main_plugin_constants.hpp"

namespace plugin {
namespace tcp_server {

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
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
            LOG_CALL(DVLOG(99))
                << kPluginName
                << " starting...";
          })
        )
      .ThenHere(FROM_HERE
        , base::BindOnce(
          &MainPluginLogic::load
          , base::Unretained(&mainPluginLogic_.value()))
        , base::IsNestedPromise{true}
      )
      .ThenHere(FROM_HERE
        , base::BindOnce(
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
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
            LOG_CALL(DVLOG(99))
              << kPluginName
              << " terminating...";
          })
      )
      .ThenHere(FROM_HERE
        , base::BindOnce(
          &MainPluginLogic::unload
          , base::Unretained(&mainPluginLogic_.value()))
        , base::IsNestedPromise{true}
      )
      .ThenHere(FROM_HERE
        , base::BindOnce(
            &base::Optional<MainPluginLogic>::reset
            , base::Unretained(&mainPluginLogic_)
          )
      )
      .ThenHere(FROM_HERE
        , base::BindOnce(
          [
          ](
          ){
            LOG_CALL(DVLOG(99))
              << kPluginName
              << " terminated...";
          })
      );
}

std::string MainPluginInterface::ipAddr() const NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::string ipAddr
    = kDefaultIpAddr;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = metadata()->configuration();

  if(configuration.hasValue(kConfIpAddr))
  {
    ipAddr =
      configuration.value(kConfIpAddr);
  }

  return ipAddr;
}

int MainPluginInterface::portNum() const NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int portNum
    = kDefaultPortNum;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = metadata()->configuration();

  if(configuration.hasValue(kConfPortNum))
  {
    base::StringToInt(
      configuration.value(kConfPortNum)
      , &portNum);
  }

  return portNum;
}

int MainPluginInterface::quitDetectionFreqMillisec() const NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int quitDetectionFreqMillisec
    = kDefaultQuitDetectionFreqMillisec;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = metadata()->configuration();

  if(configuration.hasValue(kConfQuitDetectionFreqMillisec))
  {
    base::StringToInt(
      configuration.value(kConfQuitDetectionFreqMillisec)
      , &quitDetectionFreqMillisec);
  }

  return quitDetectionFreqMillisec;
}

int MainPluginInterface::quitDetectionDebugTimeoutMillisec() const NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int quitDetectionDebugTimeoutMillisec
    = kDefaultQuitDetectionDebugTimeoutMillisec;

  const ::Corrade::Utility::ConfigurationGroup& configuration
    = metadata()->configuration();

  if(configuration.hasValue(kConfQuitDetectionDebugTimeoutMillisec))
  {
    base::StringToInt(
      configuration.value(kConfQuitDetectionDebugTimeoutMillisec)
      , &quitDetectionDebugTimeoutMillisec);
  }

  return quitDetectionDebugTimeoutMillisec;
}

} // namespace tcp_server
} // namespace plugin
