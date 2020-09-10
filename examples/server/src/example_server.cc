#include "example_server.hpp" // IWYU pragma: associated

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/util/lock_with_check.hpp>
#include <flexnet/util/periodic_validate_until.hpp>

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
#include <basis/ECS/sequence_local_context.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace backend {

ExampleServer::ExampleServer(
  const std::string& ip_addr
  , const unsigned short port_num)
  : tcpEndpoint_{
      ::boost::asio::ip::make_address(ip_addr)
      , port_num}
    , asioRegistry_{REFERENCED(ioc_)}
    , listener_{
        ioc_
        , EndpointType{tcpEndpoint_}
        , REFERENCED(asioRegistry_)}
    , signals_set_(ioc_, SIGINT, SIGTERM)
    , mainLoopRunner_{
        base::MessageLoop::current()->task_runner()}
    , asio_thread_1("asio_thread_1")
    , asio_thread_2("asio_thread_2")
    , asio_thread_3("asio_thread_3")
    , asio_thread_4("asio_thread_4")
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

#if defined(SIGQUIT)
  signals_set_.add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  auto sigQuitCallback
    = [this]
      (boost::system::error_code const&, int)
      {
        LOG_CALL(DVLOG(99));

        LOG(INFO)
          << "got stop signal";
        {
          DCHECK_CUSTOM_THREAD_GUARD(mainLoopRunner_);
          DCHECK(mainLoopRunner_);
          (mainLoopRunner_)->PostTask(FROM_HERE
            , base::BindRepeating(
                &ExampleServer::hangleQuitSignal
                , base::Unretained(this)));
        }
      };


  signals_set_.async_wait(base::rvalue_cast(sigQuitCallback));
}

void ExampleServer::hangleQuitSignal()
{
  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK_CUSTOM_THREAD_GUARD(mainLoopRunner_);

  // stop accepting of new connections
  base::PostPromise(FROM_HERE
    , UNOWNED_LIFETIME(mainLoopRunner_.get())
    , base::BindOnce(
        &ExampleServer::stopAcceptors
        , base::Unretained(this)
      )
    , /*nestedPromise*/ true
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      [
      ](
        const ::util::Status& stopAcceptorResult
      ){
         LOG_CALL(DVLOG(99));

         if(!stopAcceptorResult.ok()) {
           LOG(ERROR)
             << "failed to stop acceptor with status: "
             << stopAcceptorResult.ToString();
           NOTREACHED();
         }
      })
  )
  // async-wait for destruction of existing connections
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &ExampleServer::promiseNetworkResourcesFreed
        , base::Unretained(this)
    )
    , /*nestedPromise*/ true
  )
  .ThenOn(mainLoopRunner_
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
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &ExampleServer::stopIOContext
        , base::Unretained(this)
    )
  )
  /// \todo use |SequenceLocalContext|
#if 0
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        // |GlobalContext| is not thread-safe,
        // so modify it only from one sequence
        &ECS::GlobalContext::unlockModification
        , base::Unretained(ECS::GlobalContext::GetInstance())
      )
  )
  /// \todo use |SequenceLocalContext| to store |ECS::SimulationRegistry|
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &ECS::GlobalContext::unset<ECS::SimulationRegistry>
        , base::Unretained(ECS::GlobalContext::GetInstance())
        , FROM_HERE
      )
  )
#endif // 0
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , run_loop_.QuitClosure());
}

ExampleServer::StatusPromise ExampleServer::stopAcceptors() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  return base::Promises::All(FROM_HERE
    /// \todo add more acceptors
    , listener_.stopAcceptorAsync());
}

void ExampleServer::updateAsioRegistry() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicAsioTaskRunner_.get());

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  if(ioc_.stopped()) // io_context::stopped is thread-safe
  {
    LOG(WARNING)
      << "skipping update of registry"
         " because of stopped io context";

    DCHECK(asioRegistry_.registry_unsafe(FROM_HERE
      , "we can access empty (unused) registry"
        " from any thread when ioc stopped").empty());

    return;
  }

  ::boost::asio::post(
    asioRegistry_.strand()
    /// \todo use base::BindFrontWrapper
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
      , REFERENCED(asioRegistry_)
    )
  );
}

void ExampleServer::setupPeriodicAsioExecutor() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicAsioTaskRunner_.get());

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  // Can not register same data type twice.
  // Forces users to call `sequenceLocalContext->unset`.
  DCHECK(!sequenceLocalContext->try_ctx<PeriodicAsioExecutorType>(FROM_HERE));
  PeriodicAsioExecutorType& result
    = sequenceLocalContext->set_once<PeriodicAsioExecutorType>(
        FROM_HERE
        , "PeriodicAsioExecutorType_" + FROM_HERE.ToString()
        , base::BindRepeating(
            &ExampleServer::updateAsioRegistry
            , base::Unretained(this))
      );

  /// \todo make period configurable
  result->startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(100));
}

void ExampleServer::deletePeriodicAsioExecutor() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicAsioTaskRunner_.get());

  base::WeakPtr<ECS::SequenceLocalContext> sequenceLocalContext
    = ECS::SequenceLocalContext::getSequenceLocalInstance(
        FROM_HERE, base::SequencedTaskRunnerHandle::Get());

  DCHECK(sequenceLocalContext);
  DCHECK(sequenceLocalContext->try_ctx<PeriodicAsioExecutorType>(FROM_HERE));
  sequenceLocalContext->unset<PeriodicAsioExecutorType>(FROM_HERE);
}

void ExampleServer::runLoop() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON(&sequence_checker_);

  prepareBeforeRunLoop();

  {
    DCHECK(!periodicAsioTaskRunner_);
    periodicAsioTaskRunner_
      = base::ThreadPool::GetInstance()->
         CreateSequencedTaskRunnerWithTraits(
           base::TaskTraits{
             base::TaskPriority::BEST_EFFORT
             , base::MayBlock()
             , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
           }
         );
  }

  periodicAsioTaskRunner_->PostTask(FROM_HERE
    , base::BindOnce(
      &ExampleServer::setupPeriodicAsioExecutor
      , base::Unretained(this)
    )
  );

  {
    base::Thread::Options options;
    asio_thread_1.StartWithOptions(options);
    asio_thread_1.task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) // io_context::stopped is thread-safe
            {
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
          , REFERENCED(ioc_)
      )
    );
    asio_thread_1.WaitUntilThreadStarted();
    DCHECK(asio_thread_1.IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_2.StartWithOptions(options);
    asio_thread_2.task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) // io_context::stopped is thread-safe
            {
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
          , REFERENCED(ioc_)
      )
    );
    asio_thread_2.WaitUntilThreadStarted();
    DCHECK(asio_thread_2.IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_3.StartWithOptions(options);
    asio_thread_3.task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) // io_context::stopped is thread-safe
            {
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
          , REFERENCED(ioc_)
      )
    );
    asio_thread_3.WaitUntilThreadStarted();
    DCHECK(asio_thread_3.IsRunning());
  }

  {
    base::Thread::Options options;
    asio_thread_4.StartWithOptions(options);
    asio_thread_4.task_runner()->PostTask(FROM_HERE
      , base::BindRepeating(
          [
          ](
            boost::asio::io_context& ioc
          ){
            if(ioc.stopped()) // io_context::stopped is thread-safe
            {
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
          , REFERENCED(ioc_)
      )
    );
    asio_thread_4.WaitUntilThreadStarted();
    DCHECK(asio_thread_4.IsRunning());
  }

  run_loop_.Run();

  DVLOG(9)
    << "Main run loop finished";

  asio_thread_1.Stop();
  DCHECK(!asio_thread_1.IsRunning());

  asio_thread_2.Stop();
  DCHECK(!asio_thread_2.IsRunning());

  asio_thread_3.Stop();
  DCHECK(!asio_thread_3.IsRunning());

  asio_thread_4.Stop();
  DCHECK(!asio_thread_4.IsRunning());

  // Wait for deletion  of `periodicAsioExecutor_`
  // on task runner associated with it
  /// \note We can not just call `periodicAsioTaskRunner_.reset()` because
  /// it is shared type and `PeriodicAsioExecutor` prolongs its lifetime
  {
    VoidPromise promise
      = base::PostPromise(FROM_HERE
          , periodicAsioTaskRunner_.get()
          , base::BindOnce(
            &ExampleServer::deletePeriodicAsioExecutor
            , base::Unretained(this)
          )
        );
    /// \note Will block current thread for unspecified time.
    base::waitForPromiseResolve(
      promise
      , base::ThreadPool::GetInstance()->
          CreateSequencedTaskRunnerWithTraits(
            base::TaskTraits{
              base::TaskPriority::BEST_EFFORT
              , base::MayBlock()
              , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
            }
        )
    );
  }
}

void ExampleServer::prepareBeforeRunLoop() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::PostPromise(FROM_HERE
    /// \note delayed execution:
    /// will be executed only when |run_loop| is running
    , base::MessageLoop::current()->task_runner().get()
    , base::BindOnce(
        &ExampleServer::configureAndRunAcceptor
        , base::Unretained(this)
    )
    , /*nestedPromise*/ true
  )
  /// \todo use |SequenceLocalContext|
#if 0
  .ThenOn(base::MessageLoop::current()->task_runner()
    , FROM_HERE
    , base::BindOnce(
        // |GlobalContext| is not thread-safe,
        // so modify it only from one sequence
        &ECS::GlobalContext::lockModification
        , base::Unretained(ECS::GlobalContext::GetInstance())
      )
  )
#endif // 0
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
  ExampleServer::promiseNetworkResourcesFreed() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON(&sequence_checker_);

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_.stopped()); // io_context::stopped is thread-safe

  // Will check periodically if `asioRegistry->empty()` and if true,
  // than promise will be resolved.
  // Periodic task will be redirected to `asio_registry.strand()`.
  basis::PeriodicValidateUntil::ValidationTaskType validationTask
    = base::BindRepeating(
        [
        ](
          boost::asio::io_context& ioc
          , ECS::AsioRegistry& asio_registry
          , COPIED() base::RepeatingClosure resolveCallback
        ){
          LOG(INFO)
            << "waiting for cleanup of asio registry...";

          /// \note you can not use `::boost::asio::post`
          /// if `ioc->stopped()`
          {
            DCHECK(!ioc.stopped()); // io_context::stopped is thread-safe
          }

          // redirect task to strand
          ::boost::asio::post(
            asio_registry.strand()
            , std::bind(
              []
              (
                ECS::AsioRegistry& asioRegistry
                , COPIED() base::RepeatingClosure resolveCallback)
              {
                LOG_CALL(DVLOG(99));

                DCHECK(asioRegistry.running_in_this_thread());

                if(asioRegistry->empty()) {
                  DVLOG(9)
                    << "registry is empty";
                  DCHECK(resolveCallback);
                  resolveCallback.Run();
                } else {
                  DVLOG(9)
                    << "registry is NOT empty";
                }
              }
              , REFERENCED(asio_registry)
              , COPIED(resolveCallback)
            )
          );
        }
        , REFERENCED(ioc_)
        , REFERENCED(asioRegistry_)
    );

  return periodicValidateUntil_.runPromise(FROM_HERE
    , basis::EndingTimeout{
        base::TimeDelta::FromSeconds(15)} // debug-only expiration time
    , basis::PeriodicCheckUntil::CheckPeriod{
        base::TimeDelta::FromSeconds(1)}
    , "destruction of allocated connections hanged" // debug-only error
    , base::rvalue_cast(validationTask)
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
  );
}

ExampleServer::VoidPromise ExampleServer::configureAndRunAcceptor() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(mainLoopRunner_);

  DCHECK_RUN_ON(&sequence_checker_);

  return listener_.configureAndRun()
  .ThenOn(mainLoopRunner_
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
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON(&sequence_checker_);

  {
    // Stop the io_context. This will cause run()
    // to return immediately, eventually destroying the
    // io_context and any remaining handlers in it.
    ioc_.stop(); // io_context::stop is thread-safe
    DCHECK(ioc_.stopped()); // io_context::stopped is thread-safe
  }
}

ExampleServer::~ExampleServer()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK(ioc_.stopped()); // io_context::stopped is thread-safe
}

} // namespace backend
