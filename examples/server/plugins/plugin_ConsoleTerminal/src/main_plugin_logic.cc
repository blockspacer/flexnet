#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

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

namespace plugin {
namespace console_terminal {

// UNIQUE type to store in sequence-local-context
using ConsolePeriodicTaskExecutor
  = util::StrongAlias<
      class ConsolePeriodicTaskExecutorTag
      , basis::PeriodicTaskExecutor
    >;

template<
  typename EventType
  , typename DispatcherType
>
static void postOnRunnerDispatcherEvent(
  scoped_refptr<base::SequencedTaskRunner> taskRunner
  , EventType&& event)
{
  taskRunner->PostTask(FROM_HERE,
    base::BindOnce(
    [
    ](
      EventType&& event
    ){
      ::backend::MainLoopRegistry::RegistryType& mainLoopRegistry
          = ::backend::MainLoopRegistry::GetInstance()->registry();

      if(!mainLoopRegistry.try_ctx<DispatcherType>())
      {
        DVLOG(99)
          << "Unable to trigger event"
             " using not-existing runner";
        return;
      }

      DispatcherType& eventDispatcher
        = mainLoopRegistry.template ctx<DispatcherType>();

      eventDispatcher->template trigger<
        EventType
      >(event);
    }
    , base::rvalue_cast(event)
    )
  );
}

static void setConsolePeriodicTaskExecutorOnSequence(
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

static void startConsolePeriodicTaskExecutorOnSequence(
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

static void unsetConsolePeriodicTaskExecutorOnSequence()
{
  LOG_CALL(DVLOG(99));

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<ConsolePeriodicTaskExecutor>(FROM_HERE));
  sequenceLocalContext->unset<ConsolePeriodicTaskExecutor>(FROM_HERE);
}

static void updateConsoleInput(
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

    postOnRunnerDispatcherEvent<
      std::string // event type
      , backend::ConsoleTerminalEventDispatcher // dispatcher type
    >(mainLoopRunner
      , base::rvalue_cast(line));
  } else {
    DVLOG(99)
      << "no console input";
  }
}

MainPluginLogic::MainPluginLogic(
  const MainPluginInterface* pluginInterface)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , pluginInterface_{REFERENCED(*pluginInterface)}
  , mainLoopRegistry_(
      ::backend::MainLoopRegistry::GetInstance())
  , consoleTerminalEventDispatcher_(
      REFERENCED(mainLoopRegistry_->registry()
        .set<::backend::ConsoleTerminalEventDispatcher>()))
  , mainLoopRunner_{
      base::MessageLoop::current()->task_runner()}
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

MainPluginLogic::~MainPluginLogic()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);
}

MainPluginLogic::VoidPromise
  MainPluginLogic::load()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::load()");

  return base::PostPromise(
    FROM_HERE
    , periodicConsoleTaskRunner_.get()
    , base::BindOnce(
        &setConsolePeriodicTaskExecutorOnSequence
        , FROM_HERE
        , periodicConsoleTaskRunner_
        , base::BindRepeating(
            &updateConsoleInput
            , mainLoopRunner_
            , periodicConsoleTaskRunner_
          )
      )
  )
  .ThenOn(periodicConsoleTaskRunner_
    , FROM_HERE
    , base::BindOnce(
        &startConsolePeriodicTaskExecutorOnSequence
        , base::TimeDelta::FromMilliseconds(
            pluginInterface_->consoleInputFreqMillisec())
      )
  );
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK(mainLoopRunner_);

  return base::PostPromise(
    FROM_HERE
    // Post our work to the strand, to prevent data race
    , periodicConsoleTaskRunner_.get()
    , base::BindOnce(&unsetConsolePeriodicTaskExecutorOnSequence)
  );
}

} // namespace console_terminal
} // namespace plugin
