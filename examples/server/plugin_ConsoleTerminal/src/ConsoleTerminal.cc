#include "plugin_interface/plugin_interface.hpp"
#include "console_terminal/console_dispatcher.hpp"

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
#include <basis/task/periodic_check.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/strong_alias.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <iostream>

namespace {

template<
  typename Type
  , typename DispatcherType
>
void triggerEventOnRunner(
  scoped_refptr<base::SequencedTaskRunner> taskRunner
  , Type&& line)
{
  taskRunner->PostTask(FROM_HERE,
    base::BindOnce(
    [
    ](
      Type&& line
    ){
      base::WeakPtr<ECS::SequenceLocalContext> mainLoopContext
        = ECS::SequenceLocalContext::getSequenceLocalInstance(
            FROM_HERE, base::MessageLoop::current()->task_runner());
      if(!mainLoopContext->try_ctx<DispatcherType>(FROM_HERE))
      {
        DVLOG(99)
          << "Unable to trigger event"
             " using not-existing runner";
        return;
      }
      DispatcherType& eventDispatcher
        = mainLoopContext->ctx<DispatcherType>(FROM_HERE);
      eventDispatcher->template trigger<
        Type
      >(line);
    }
    , base::rvalue_cast(line)
    )
  );
}

// UNIQUE type to store in sequence-local-context
using ConsolePeriodicTaskExecutor
  = util::StrongAlias<
      class ConsolePeriodicTaskExecutorTag
      , basis::PeriodicTaskExecutor
    >;

void setConsolePeriodicTaskExecutorOnSequence(
  const base::Location& from_here
  , scoped_refptr<base::SequencedTaskRunner> task_runner
  , COPIED() base::RepeatingClosure updateCallback)
{
  LOG_CALL(DVLOG(99));

  DCHECK(task_runner
    && task_runner->RunsTasksInCurrentSequence());

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        from_here, task_runner);

  DCHECK(sequenceLocalContext);
  // Can not register same data type twice.
  // Forces users to call `sequenceLocalContext->unset`.
  DCHECK(!sequenceLocalContext->try_ctx<ConsolePeriodicTaskExecutor>(FROM_HERE));
  ConsolePeriodicTaskExecutor& result
    = sequenceLocalContext->set_once<ConsolePeriodicTaskExecutor>(
        from_here
        , "Timeout.ConsolePeriodicTaskExecutor." + from_here.ToString()
        , std::move(updateCallback)
      );
  ignore_result(result);
}

void startConsolePeriodicTaskExecutorOnSequence(
  const base::TimeDelta& endTimeDelta)
{
  LOG_CALL(DVLOG(99));

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<ConsolePeriodicTaskExecutor>(FROM_HERE));
  ConsolePeriodicTaskExecutor& executor
    = sequenceLocalContext->ctx<ConsolePeriodicTaskExecutor>(FROM_HERE);

  executor->startPeriodicTimer(
    endTimeDelta);
}

void unsetConsolePeriodicTaskExecutorOnSequence()
{
  LOG_CALL(DVLOG(99));

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<ConsolePeriodicTaskExecutor>(FROM_HERE));
  sequenceLocalContext->unset<ConsolePeriodicTaskExecutor>(FROM_HERE);
}

} // namespace

namespace plugin {
namespace console_terminal {

class ConsoleTerminal
  final
  : public ::plugin::PluginInterface {
 public:
  explicit ConsoleTerminal(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ALLOW_THIS_IN_INITIALIZER_LIST(
        weak_ptr_factory_(COPIED(this)))
    , ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
    , periodicConsoleTaskRunner_(
        base::ThreadPool::GetInstance()->
          CreateSequencedTaskRunnerWithTraits(
            base::TaskTraits{
              base::TaskPriority::BEST_EFFORT
              , base::MayBlock()
              , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
            }
          ))
  {
    LOG_CALL(DVLOG(99));

    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~ConsoleTerminal()
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

    TRACE_EVENT0("headless", "plugin::ConsoleTerminal::load()");

     LOG_CALL(DVLOG(99))
      << " ConsoleTerminal starting...";

    ignore_result(
     consoleTerminalEventDispatcher_.emplace(
       FROM_HERE
       , "ConsoleTerminalEventDispatcher_" + FROM_HERE.ToString()
     )
    );

    return base::PostPromise(
      FROM_HERE
      , periodicConsoleTaskRunner_.get()
      , base::BindOnce(
          &setConsolePeriodicTaskExecutorOnSequence
          , FROM_HERE
          , periodicConsoleTaskRunner_
          , base::BindRepeating(
              &ConsoleTerminal::update
              , mainLoopRunner_
              , periodicConsoleTaskRunner_
            )
        )
    )
    .ThenOn(periodicConsoleTaskRunner_
      , FROM_HERE
      , base::BindOnce(
          &startConsolePeriodicTaskExecutorOnSequence
          /// \todo make configurable
          , base::TimeDelta::FromMilliseconds(100)
        )
    );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::ConsoleTerminal::unload()");

    DLOG(INFO)
      << "unloaded plugin with title = "
      << title()
      << " and description = "
      << description().substr(0, 100)
      << "...";

    return base::PostPromise(
      FROM_HERE
      // Post our work to the strand, to prevent data race
      , periodicConsoleTaskRunner_.get()
      , base::BindOnce(&unsetConsolePeriodicTaskExecutorOnSequence)
    );
  }

  static void update(
    scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner
    , scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner) NO_EXCEPTION
  {
    LOG_CALL(DVLOG(99));

    DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicConsoleTaskRunner.get());

    std::string line;

    // use cin.peek to check if there is anything to read,
    // to exit if no input
    if(!std::cin.eof())
    {
      /// \todo std::getline blocks without timeout, so add timeout like in
      /// github.com/throwaway-github-account-alex/coding-exercise/blob/9ccd3d04ffa5569a2004ee195171b643d252514b/main.cpp#L12
      /// or stackoverflow.com/a/34994150
      /// i.e. use STDIN_FILENO and poll in Linux
      /// In Windows, you will need to use timeSetEvent or WaitForMultipleEvents
      /// or need the GetStdHandle function to obtain a handle
      /// to the console, then you can use WaitForSingleObject
      /// to wait for an event to happen on that handle, with a timeout
      /// see stackoverflow.com/a/21749034
      /// see stackoverflow.com/questions/40811438/input-with-a-timeout-in-c
      std::getline(std::cin, line);

      DVLOG(99)
        << "console input: "
        << line;

      triggerEventOnRunner<
        std::string, backend::ConsoleTerminalEventDispatcher
      >(mainLoopRunner
        , base::rvalue_cast(line));
    } else {
      DVLOG(99)
        << "no console input";
    }
  }

private:
  base::WeakPtrFactory<
    ConsoleTerminal
  > weak_ptr_factory_;

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  basis::ScopedSequenceCtxVar<
    backend::ConsoleTerminalEventDispatcher
  > consoleTerminalEventDispatcher_;

  // Task sequence used to update text input from console terminal.
  scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ConsoleTerminal);
};

} // namespace console_terminal
} // namespace plugin

REGISTER_PLUGIN(/*name*/ ConsoleTerminal
    , /*className*/ plugin::console_terminal::ConsoleTerminal
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
