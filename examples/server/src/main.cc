#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/util/lock_with_check.hpp>

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

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

using namespace flexnet;

namespace {

static const base::FilePath::CharType kIcuDataFileName[]
  = FILE_PATH_LITERAL(R"raw(./resources/icu/optimal/icudt64l.dat)raw");

static const base::FilePath::CharType kTraceReportFileName[]
  = FILE_PATH_LITERAL(R"raw(trace_report.json)raw");

// Check to see that we are being called on only one thread.
// Useful during app initialization because sequence checkers
// may be not available before app initialized.
MUST_USE_RETURN_VALUE
bool isCalledOnSameSingleThread()
{
  static base::PlatformThreadId thread_id = 0;
  if (!thread_id) {
    thread_id = base::PlatformThread::CurrentId();
  }
  return base::PlatformThread::CurrentId() == thread_id;
}

static base::Promise<void, base::NoReject> promiseCleanupConnectionResources(
  ECS::AsioRegistry& asio_registry
  , scoped_refptr<base::SequencedTaskRunner> waitCleanupRunner)
{
  LOG_CALL(DVLOG(9));

  // promise will be resolved when `registry.empty()`
  base::ManualPromiseResolver<
      void, base::NoReject
    > promiseResolver = base::ManualPromiseResolver<
      void, base::NoReject
    >(FROM_HERE);

  DCHECK(waitCleanupRunner
    && waitCleanupRunner->RunsTasksInCurrentSequence());

  // check periodically if `registry.empty()`
  basis::setPeriodicTaskExecutorOnAsioExecutor(
    FROM_HERE
    , waitCleanupRunner
    , asio_registry.ref_strand(FROM_HERE)
    , base::BindRepeating(
        [
        ](
          ECS::AsioRegistry& asio_registry
          , base::OnceClosure&& resolveCallback
        ){
          LOG(INFO)
            << "waiting for cleanup of asio registry...";

          DCHECK(asio_registry.running_in_this_thread());

          if(asio_registry->empty()) {
            DVLOG(9)
              << "registry is empty";
            DCHECK(resolveCallback);
            base::rvalue_cast(resolveCallback).Run();
          } else {
            DVLOG(9)
              << "registry is NOT empty";
          }
        }
        , REFERENCED(asio_registry)
        , promiseResolver.GetRepeatingResolveCallback()
    ));

  basis::startPeriodicTaskExecutorOnSequence(
    base::TimeDelta::FromMilliseconds(500));

  return promiseResolver.promise();
}

// init common application systems,
// initialization order matters!
MUST_USE_RETURN_VALUE
base::Optional<int> initEnv(
  int argc
  , char* argv[]
  , basis::ScopedBaseEnvironment& base_env
  )
{
  DCHECK(isCalledOnSameSingleThread());

  base::FilePath dir_exe;
  if (!base::PathService::Get(base::DIR_EXE, &dir_exe)) {
    NOTREACHED();
    return EXIT_FAILURE;
  }

  // ScopedBaseEnvironment
  {
    const bool envCreated
      = base_env.init(
          argc
          , argv
          , false // AutoStartTracer
          , "" // tracingCategories
          , dir_exe // current working dir
          , kIcuDataFileName
          , kTraceReportFileName
          /// \note number of threads in global thread pool
          , 11 // threadsNum
          );
    if(!envCreated) {
      LOG(ERROR)
        << "Unable to create base environment";
      return
        EXIT_FAILURE;
    }
  }

  // Stores vector of arbitrary typed objects,
  // each object can be found by its type (using entt::type_info).
  ECS::GlobalContext* globals
    = ECS::GlobalContext::GetInstance();
  DCHECK(globals);

  // |GlobalContext| is not thread-safe,
  // so modify it only from one sequence
  globals->unlockModification();

  // main ECS registry
  ECS::SimulationRegistry& enttManager
    = globals->set_once<ECS::SimulationRegistry>(FROM_HERE,
        "Ctx_SimulationRegistry");

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<base::SequencedTaskRunner> entt_task_runner =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  // ECS registry is not thread-safe
  // i.e. manipulate sessions in single sequence.
  enttManager.set_task_runner(entt_task_runner);

  return base::nullopt;
}

} // namespace

class ExampleServer
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  using EndpointType
    = ws::Listener::EndpointType;

  using FakeLockRunType = bool();

  using FakeLockPolicy = basis::FakeLockPolicyDebugOnly;

 public:
  ExampleServer();

  ~ExampleServer();

  void runLoop() NO_EXCEPTION;

  // Loads configurations,
  // required before first run.
  void prepare() NO_EXCEPTION;

  NOT_THREAD_SAFE_FUNCTION()
  void updateAsioRegistry() NO_EXCEPTION;

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptors() NO_EXCEPTION;

  MUST_USE_RETURN_VALUE
  VoidPromise configureAndRunAcceptor() NO_EXCEPTION;

  /// \note make sure connections recieved `stop` message
  /// and acceptors stopped
  /// i.e. do not allocate new connections
  MUST_USE_RETURN_VALUE
  VoidPromise waitNetworkResourcesFreed() NO_EXCEPTION;

  void stopIOContext() NO_EXCEPTION;

 private:
  /// \note It is not real lock, only annotated as lock.
  /// It just calls callback on scope entry AND exit.
  basis::FakeLockWithCheck<FakeLockRunType>
    fakeLockToSequence {
      BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
    };

  /// \todo replace with Invalidatable<>
  // The io_context is required for all I/O
  basis::CheckedOptional<
    boost::asio::io_context
    , basis::CheckedOptionalPolicy::DebugOnly
  > ioc_{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      , util::CheckedOptionalPermissions::Readable
      , base::in_place};

  const EndpointType tcpEndpoint_
    GUARDED_BY(fakeLockToSequence);

  /// \todo SSL support
  // GLOBAL_THREAD_SAFE_LIFETIME()
  // ::boost::asio::ssl::context ctx_
  //   {::boost::asio::ssl::context::tlsv12};

  basis::CheckedOptional<
    ECS::AsioRegistry
    , basis::CheckedOptionalPolicy::DebugOnly
  > asioRegistry_{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , util::UnownedPtr<ws::Listener::IoContext>(&(*ioc_))};

  ws::Listener listener_
    GUARDED_BY(fakeLockToSequence);

  basis::CheckedOptional<
    base::RunLoop
    , basis::CheckedOptionalPolicy::DebugOnly
  > run_loop_{
      BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
      , util::CheckedOptionalPermissions::All
      , base::in_place
    };

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  basis::CheckedOptional<
    boost::asio::signal_set
    , basis::CheckedOptionalPolicy::DebugOnly
  > signals_set_{
      BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
      , util::CheckedOptionalPermissions::All
      , base::in_place
      , *ioc_ /// \note will not handle signals if ioc stopped
      , SIGINT
      , SIGTERM
    };

  basis::CheckedOptional<
    scoped_refptr<base::SingleThreadTaskRunner>
    , basis::CheckedOptionalPolicy::DebugOnly
  > mainLoopRunner_{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , base::MessageLoop::current()->task_runner()};

  basis::CheckedOptional<
    base::Thread
    , basis::CheckedOptionalPolicy::DebugOnly
  > asio_thread_1{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , "asio_thread_1"};

  basis::CheckedOptional<
    base::Thread
    , basis::CheckedOptionalPolicy::DebugOnly
  > asio_thread_2{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , "asio_thread_2"};

  basis::CheckedOptional<
    base::Thread
    , basis::CheckedOptionalPolicy::DebugOnly
  > asio_thread_3{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , "asio_thread_3"};

  basis::CheckedOptional<
    base::Thread
    , basis::CheckedOptionalPolicy::DebugOnly
  > asio_thread_4{
      // it safe to read value from any thread
      // because its storage expected to be not modified
      basis::VerifyNothing::Repeatedly()
      // disallow `emplace` for thread-safety reasons
      , util::CheckedOptionalPermissions::Readable
      , base::in_place
      , "asio_thread_4"};

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

ExampleServer::ExampleServer()
  : tcpEndpoint_{
      ::boost::asio::ip::make_address("127.0.0.1")
      /// \todo make port configurable
      , (unsigned short) 8085}
    , listener_{
        util::UnownedPtr<ws::Listener::IoContext>(&(*ioc_))
        , EndpointType{tcpEndpoint_}
        , RAW_REFERENCED(*asioRegistry_)}
{
  LOG_CALL(DVLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);

#if defined(SIGQUIT)
  signals_set_->add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  auto sigQuitCallback
    = [this]
      (boost::system::error_code const&, int)
      {
        LOG_CALL(DVLOG(9));

        DCHECK(*mainLoopRunner_);

        LOG(INFO)
          << "got stop signal";

        // stop accepting of new connections
        base::PostPromise(FROM_HERE
          , UNOWNED_LIFETIME(mainLoopRunner_->get())
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::stopAcceptors)
              , base::Unretained(this))
        )
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
            [
            ](
              const ::util::Status& stopAcceptorResult
            ){
               LOG_CALL(DVLOG(9));

               if(!stopAcceptorResult.ok()) {
                 LOG(ERROR)
                   << "failed to stop acceptor with status: "
                   << stopAcceptorResult.ToString();
                 NOTREACHED();
               }
            })
        )
        // async-wait for destruction of existing connections
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::waitNetworkResourcesFreed)
              , base::Unretained(this)
          )
        )
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
            []
            ()
            {
              LOG(INFO)
                << "stopping io context";
            }
          )
        )
        // stop io context
        /// \note you can not use `::boost::asio::post`
        /// if `ioc_->stopped()`
        /// i.e. can not use strand of registry e.t.c.
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::stopIOContext)
              , base::Unretained(this)
          )
        )
        /*// reset |listener_|
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              &basis::CheckedOptional<
                ws::Listener
                , basis::CheckedOptionalPolicy::DebugOnly
              >::reset_unsafe
              , base::Unretained(&listener_)
              , FROM_HERE
              , "safe to use after SIGQUIT"
              , base::DoNothing::Once()
            )
        )*/
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              // |GlobalContext| is not thread-safe,
              // so modify it only from one sequence
              &ECS::GlobalContext::unlockModification
              , base::Unretained(ECS::GlobalContext::GetInstance())
            )
        )
        // delete |ECS::SimulationRegistry|
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              &ECS::GlobalContext::unset<ECS::SimulationRegistry>
              , base::Unretained(ECS::GlobalContext::GetInstance())
              , FROM_HERE
            )
        )
        .ThenOn(*mainLoopRunner_
          , FROM_HERE
          , run_loop_.ref_value_unsafe(
              FROM_HERE
              , "safe to use after SIGQUIT"
              , base::DoNothing::Once())
            .QuitClosure());
      };

  signals_set_->async_wait(base::rvalue_cast(sigQuitCallback));
}

ExampleServer::StatusPromise ExampleServer::stopAcceptors() NO_EXCEPTION
{
  LOG_CALL(DVLOG(9));

  basis::AutoFakeLockWithCheck<FakeLockPolicy, FakeLockRunType>
    auto_lock(fakeLockToSequence);

  return base::Promises::All(FROM_HERE
    /// \todo add more acceptors
    , listener_.stopAcceptorAsync());
}

void ExampleServer::updateAsioRegistry() NO_EXCEPTION
{
  ECS::AsioRegistry& registry = *asioRegistry_;

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  if(ioc_->stopped())
  {
    LOG(WARNING)
      << "skipping update of registry"
         " because of stopped io context";

    DCHECK(asioRegistry_.ref_value_unsafe(
              FROM_HERE
              , "safe to use after ioc_->stopped()"
                " i.e. registry must no more used"
              , base::DoNothing::Once())->empty());

    return;
  }

  ::boost::asio::post(
    registry.ref_strand(FROM_HERE)
    , ::boost::beast::bind_front_handler([
      ](
        ECS::AsioRegistry& asio_registry
      ){
        DVLOG(9)
          << "updating asio registry...";

        DCHECK(
          asio_registry.running_in_this_thread());

        ECS::updateNewConnections(asio_registry);

        ECS::updateSSLDetection(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateClosingSockets(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateUnusedSystem(asio_registry);

        /// \todo cutomizable cleanup period
        ECS::updateCleanupSystem(asio_registry);
      }
      , REFERENCED(registry)
    )
  );
}

void ExampleServer::runLoop() NO_EXCEPTION
{
  LOG_CALL(DVLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  /// \note will stop periodic timer on scope exit
  basis::PeriodicTaskExecutor periodicAsioExecutor(
    base::ThreadPool::GetInstance()->
      CreateSequencedTaskRunnerWithTraits(
        base::TaskTraits{
          base::TaskPriority::BEST_EFFORT
          , base::MayBlock()
          , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
        }
      )
    , base::BindRepeating(
        &ExampleServer::updateAsioRegistry
        , base::Unretained(this))
  );

  periodicAsioExecutor.startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(100));

  {
    base::Thread::Options options;
    asio_thread_1->StartWithOptions(options);
    asio_thread_1->task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) {
              LOG(INFO)
                << "skipping update of stopped io context";
              return;
            }

            // we want to loop |ioc| forever
            // i.e. until manual termination
            boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard
              = boost::asio::make_work_guard(ioc);

            /// \note loops forever (if has work) and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();

            LOG(INFO)
              << "stopped io context thread";
          }
          , REFERENCED(*ioc_)
      )
    );
    asio_thread_1->WaitUntilThreadStarted();
    DCHECK(asio_thread_1->IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_2->StartWithOptions(options);
    asio_thread_2->task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) {
              LOG(INFO)
                << "skipping update of stopped io context";
              return;
            }

            // we want to loop |ioc| forever
            // i.e. until manual termination
            boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard
              = boost::asio::make_work_guard(ioc);

            /// \note loops forever (if has work) and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();

            LOG(INFO)
              << "stopped io context thread";
          }
          , REFERENCED(*ioc_)
      )
    );
    asio_thread_2->WaitUntilThreadStarted();
    DCHECK(asio_thread_2->IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_3->StartWithOptions(options);
    asio_thread_3->task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) {
              LOG(INFO)
                << "skipping update of stopped io context";
              return;
            }

            // we want to loop |ioc| forever
            // i.e. until manual termination
            boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard
              = boost::asio::make_work_guard(ioc);

            /// \note loops forever (if has work) and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();

            LOG(INFO)
              << "stopped io context thread";
          }
          , REFERENCED(*ioc_)
      )
    );
    asio_thread_3->WaitUntilThreadStarted();
    DCHECK(asio_thread_3->IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_4->StartWithOptions(options);
    asio_thread_4->task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) {
              LOG(INFO)
                << "skipping update of stopped io context";
              return;
            }

            // we want to loop |ioc| forever
            // i.e. until manual termination
            boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard
              = boost::asio::make_work_guard(ioc);

            /// \note loops forever (if has work) and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();

            LOG(INFO)
              << "stopped io context thread";
          }
          , REFERENCED(*ioc_)
      )
    );
    asio_thread_4->WaitUntilThreadStarted();
    DCHECK(asio_thread_4->IsRunning());
  }

  run_loop_->Run();

  DVLOG(9)
    << "Main run loop finished";

  asio_thread_1->Stop();
  DCHECK(!asio_thread_1->IsRunning());

  asio_thread_2->Stop();
  DCHECK(!asio_thread_2->IsRunning());

  asio_thread_3->Stop();
  DCHECK(!asio_thread_3->IsRunning());

  asio_thread_4->Stop();
  DCHECK(!asio_thread_4->IsRunning());
}

void ExampleServer::prepare() NO_EXCEPTION
{
  base::PostPromise(FROM_HERE
        /// \note delayed execution:
        /// will be executed only when |run_loop| is running
        , base::MessageLoop::current()->task_runner().get()
        , base::BindOnce(
            NESTED_PROMISE(&ExampleServer::configureAndRunAcceptor)
            , base::Unretained(this)
        )
    )
  .ThenOn(base::MessageLoop::current()->task_runner()
    , FROM_HERE
    , base::BindOnce(
        // |GlobalContext| is not thread-safe,
        // so modify it only from one sequence
        &ECS::GlobalContext::lockModification
        , base::Unretained(ECS::GlobalContext::GetInstance())
      )
  )
  .ThenOn(base::MessageLoop::current()->task_runner()
    , FROM_HERE
    , base::BindOnce(
      [
      ](
      ){
        LOG(INFO)
          << "server is running";
      }
  ));
}

ExampleServer::VoidPromise
  ExampleServer::waitNetworkResourcesFreed() NO_EXCEPTION
{
  LOG_CALL(DVLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(base::ThreadPool::GetInstance());
  // wait and signal on different task runners
  scoped_refptr<base::SequencedTaskRunner> timeout_task_runner =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  ignore_result(base::PostPromise(FROM_HERE
  , timeout_task_runner.get()
  , base::BindOnce(
    // limit execution time
    &basis::setPeriodicTimeoutCheckerOnSequence
    , FROM_HERE
    , timeout_task_runner
    , basis::EndingTimeout{
        base::TimeDelta::FromSeconds(30)}
    , basis::PeriodicCheckUntil::CheckPeriod{
        base::TimeDelta::FromSeconds(1)}
    , "destruction of allocated connections hanged")));

  scoped_refptr<base::SequencedTaskRunner> waitCleanupRunner
    = base::ThreadPool::GetInstance()->
      CreateSequencedTaskRunnerWithTraits(
        base::TaskTraits{
          base::TaskPriority::BEST_EFFORT
          , base::MayBlock()
          , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
        }
      );

  return base::PostPromise(
    FROM_HERE
    // Post our work to the strand, to prevent data race
    , waitCleanupRunner.get()
    , base::BindOnce(
        NESTED_PROMISE(&promiseCleanupConnectionResources)
        /// \note take care of thread-safety
        , REFERENCED(*asioRegistry_)
        , SHARED_LIFETIME(waitCleanupRunner))
    )
    .ThenOn(waitCleanupRunner
      , FROM_HERE
      , base::BindOnce(&basis::unsetPeriodicTaskExecutorOnSequence)
    )
    /// \note promise has shared lifetime,
    /// so we expect it to exist until (at least)
    /// it is resolved using `GetRepeatingResolveCallback`
    // reset check of execution time
    .ThenOn(timeout_task_runner
      , FROM_HERE
      , base::BindOnce(&basis::unsetPeriodicTimeoutCheckerOnSequence)
    )
    .ThenHere(
      FROM_HERE
      , base::BindOnce(
        []
        ()
        {
          LOG(INFO)
            << "finished cleanup of network entities";
        }
      )
    )
    ;
}

ExampleServer::VoidPromise ExampleServer::configureAndRunAcceptor() NO_EXCEPTION
{
  LOG_CALL(DVLOG(9));

  basis::AutoFakeLockWithCheck<FakeLockPolicy, FakeLockRunType>
    auto_lock(fakeLockToSequence);

  return listener_.configureAndRun()
  .ThenOn(*mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      [
      ](
      ){
        LOG(INFO)
          << "websocket listener is running";
      }
  ));
}

void ExampleServer::stopIOContext() NO_EXCEPTION
{
  LOG_CALL(DVLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Stop the io_context. This will cause run()
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  ALWAYS_THREAD_SAFE(ioc_->stop());
  DCHECK(ALWAYS_THREAD_SAFE(ioc_->stopped()));
}

ExampleServer::~ExampleServer()
{
  LOG_CALL(DVLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(
    ALWAYS_THREAD_SAFE(ioc_->stopped())
  );
}

int main(int argc, char* argv[])
{
  // stores basic requirements, like thread pool, logging, etc.
  basis::ScopedBaseEnvironment base_env;

  // init common application systems,
  // initialization order matters!
  {
    base::Optional<int> exit_code = initEnv(
      argc
      , argv
      , base_env);
    if(exit_code.has_value()) {
      LOG(WARNING)
        << "exited during environment creation";
      return exit_code.value();
    }
  }

  ExampleServer exampleServer;

  exampleServer.prepare();

  exampleServer.runLoop();

  LOG(INFO)
    << "server is quitting";

  return
    EXIT_SUCCESS;
}
