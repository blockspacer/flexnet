#include "plugin_interface/plugin_interface.hpp"
#include "console_terminal/console_dispatcher.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"

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
namespace basic_console_commands {

class ConsoleInputHandler
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

 public:
  ConsoleInputHandler();

  ~ConsoleInputHandler();

  void handleConsoleInput(const std::string& line);
    /// \todo
    ///RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(FUNC_GUARD(handleConsoleInput));

 private:
  SET_WEAK_POINTERS(ConsoleInputHandler);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(mainLoopRunner_));

  base::WeakPtr<ECS::SequenceLocalContext> mainLoopContext_;
    GUARDED_BY(sequence_checker_);

  util::UnownedRef<
    ::backend::ConsoleTerminalEventDispatcher
  > consoleTerminalEventDispatcher_;
    /// \todo
    ///SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(ioc_));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ConsoleInputHandler);
};

ConsoleInputHandler::ConsoleInputHandler()
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
    , mainLoopRunner_{
        base::MessageLoop::current()->task_runner()}
    , mainLoopContext_{
        ECS::SequenceLocalContext::getSequenceLocalInstance(
          FROM_HERE, base::MessageLoop::current()->task_runner())}
    , consoleTerminalEventDispatcher_(
        REFERENCED(mainLoopContext_->ctx<
          ::backend::ConsoleTerminalEventDispatcher
        >(FROM_HERE)))
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  (*consoleTerminalEventDispatcher_)->sink<
    std::string
  >().connect<&ConsoleInputHandler::handleConsoleInput>(this);
}

ConsoleInputHandler::~ConsoleInputHandler()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  (*consoleTerminalEventDispatcher_)->sink<
    std::string
  >().disconnect<&ConsoleInputHandler::handleConsoleInput>(this);
}

void ConsoleInputHandler::handleConsoleInput(
  const std::string& line)
{
  LOG_CALL(DVLOG(99));

  if (line == "stop")
  {
    DVLOG(9)
      << "got `stop` console command";

    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(mainLoopRunner_));
    DCHECK(mainLoopRunner_);
    (mainLoopRunner_)->PostTask(FROM_HERE
      , base::BindOnce(
        [
        ](
        ){
           // send termination event
           ::backend::AppState& appState =
             ::backend::MainLoopRegistry::GetInstance()->registry()
               .ctx<::backend::AppState>();

           ::util::Status result =
             appState.processStateChange(
               FROM_HERE
               , ::backend::AppState::TERMINATE);

           DCHECK(result.ok());
        }
      ));
  }
}

class BasicConsoleCommands
  final
  : public ::plugin::PluginInterface {
 public:
  explicit BasicConsoleCommands(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
  {
    LOG_CALL(DVLOG(99));

    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~BasicConsoleCommands()
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

    TRACE_EVENT0("headless", "plugin::BasicConsoleCommands::load()");

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
            basis::ScopedSequenceCtxVar<ConsoleInputHandler>& consoleInputHandler
          ){
             LOG_CALL(DVLOG(99))
              << " BasicConsoleCommands starting...";

             ignore_result(
               consoleInputHandler.emplace(
                 FROM_HERE
                 , "ConsoleInputHandler_" + FROM_HERE.ToString()
               )
             );
          }
          , REFERENCED(consoleInputHandler_))
      );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::BasicConsoleCommands::unload()");

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
            basis::ScopedSequenceCtxVar<ConsoleInputHandler>& consoleInputHandler
          ){
             LOG_CALL(DVLOG(99))
              << " BasicConsoleCommands terminating...";

             consoleInputHandler.reset();
          }
          , REFERENCED(consoleInputHandler_))
      );
  }

private:
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  basis::ScopedSequenceCtxVar<
    ConsoleInputHandler
  > consoleInputHandler_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(BasicConsoleCommands);
};

} // namespace basic_console_commands
} // namespace plugin

REGISTER_PLUGIN(/*name*/ BasicConsoleCommands
    , /*className*/ plugin::basic_console_commands::BasicConsoleCommands
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
