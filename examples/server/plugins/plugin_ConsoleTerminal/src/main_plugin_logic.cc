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

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#if defined (_POSIX)
#include <termios.h>
#if defined (__APPLE__)
#include <sys/ioctl.h>
#endif // __APPLE__
#endif // _POSIX

#if defined (_WIN)
#include <windows.h>
#endif // _WIN

namespace plugin {
namespace console_terminal {

// UNIQUE type to store in sequence-local-context
STRONGLY_TYPED(basis::PeriodicTaskExecutor, ConsolePeriodicTaskExecutor);

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

#if defined (__linux__) || defined(OS_LINUX) || defined(OS_CHROMEOS)
// if != 0, then there is data to be read on stdin
// see https://stackoverflow.com/a/7012354
int kbhit()
{
  // timeout structure passed into select
  struct timeval tv;
  // fd_set passed into select
  fd_set fds;
  // Set up the timeout.  here we can wait for 1 second
  tv.tv_sec = 1;
  tv.tv_usec = 0;

  // Zero out the fd_set - make sure it's pristine
  FD_ZERO(&fds);
  // Set the FD that we want to read
  FD_SET(STDIN_FILENO, &fds); //STDIN_FILENO is 0
  // select takes the last file descriptor value + 1 in the fdset to check,
  // the fdset for reads, writes, and errors.  We are only passing in reads.
  // the last parameter is the timeout.  select will return if an FD is ready or
  // the timeout has occurred
  select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
  // return 0 if STDIN is not ready to be read.
  return FD_ISSET(STDIN_FILENO, &fds);
}
#endif

/// \note std::getline blocks without timeout, so prefer to use version with timeout like in
/// github.com/throwaway-github-account-alex/coding-exercise/blob/9ccd3d04ffa5569a2004ee195171b643d252514b/main.cpp#L12
/// or stackoverflow.com/a/34994150
/// https://github.com/mjdehaydu/HashCatClone/blob/bea228dabe79dfce0d657ad0cc8fb49c259ab46d/src/terminal.c#L363
/// i.e. use STDIN_FILENO and poll in Linux
/// In Windows, you will need to use timeSetEvent or WaitForMultipleEvents
/// or need the GetStdHandle function to obtain a handle
/// to the console, then you can use WaitForSingleObject
/// to wait for an event to happen on that handle, with a timeout
/// see stackoverflow.com/a/21749034
/// see stackoverflow.com/questions/40811438/input-with-a-timeout-in-c
static void updateConsoleInput(
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner
  , scoped_refptr<base::SequencedTaskRunner> periodicConsoleTaskRunner) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicConsoleTaskRunner.get());

  std::string line;

  /// \todo Read from standard input without blocking the thread.
  /// libuv is a cross-platform C library for asynchronous I/O.
  /// It uses an event loop to do things like read from standard input
  /// without blocking the thread. libuv is what powers Node.JS and others.
  /// see `uv_tty_init`, `uv_read_start`, `uv_read_stop`
#if defined (__linux__) || defined(OS_LINUX) || defined(OS_CHROMEOS) \
   || defined(OS_WIN)
  // Check if there is anything in the input
  // using a non-blocking read (kbhit()).
  // This is used for cases where we do not need too much
  // of a fine-grained control over time.
  /// \note depending on the delta, the approach may consume a lot of CPU
  /// and may be inefficient. For example, if delta=10ms, the thread
  /// will be woken up 100 times every second
  /// and it will be not efficient, especially
  /// when users do not type characters on their keyboard that fast.
  /// see https://stackoverflow.com/a/40812107
  /// \problem: thats just a hack,
  /// _kbhit() returns only true if you use your hardware keyboard.
  /// If the input to your program comes from another process then _kbhit() blocks.
  if (!kbhit()) {
    DVLOG(99)
      << "no data in input stream";
  }
  else
#else
#warning "no kbhit defined for platform"
#endif
  if(!std::cin.eof())
  {
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
