#include "example_server.hpp" // IWYU pragma: associated
#include "console/console_input_updater.hpp"
#include "console/console_terminal_on_sequence.hpp"
#include "console/console_feature_list.hpp"
#include "net/network_entity_updater_on_sequence.hpp"
#include "util/ECS/execute_and_emplace.hpp"

#include "ECS/systems/accept_connection_result.hpp"
#include "ECS/systems/cleanup.hpp"
#include "ECS/systems/ssl_detect_result.hpp"
#include "ECS/systems/unused.hpp"
#include "ECS/systems/close_socket.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/http/http_channel.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/guid.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/feature_list.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>
#include <base/trace_event/memory_dump_manager.h>
#include <base/trace_event/heap_profiler.h>
#include <base/trace_event/heap_profiler_allocation_context_tracker.h>
#include <base/trace_event/heap_profiler_event_filter.h>
#include <base/sampling_heap_profiler/sampling_heap_profiler.h>
#include <base/sampling_heap_profiler/poisson_allocation_sampler.h>
#include <base/sampling_heap_profiler/module_cache.h>
#include <base/profiler/frame.h>
#include <base/trace_event/malloc_dump_provider.h>
#include <base/trace_event/memory_dump_provider.h>
#include <base/trace_event/memory_dump_scheduler.h>
#include <base/trace_event/memory_infra_background_whitelist.h>
#include <base/trace_event/process_memory_dump.h>
#include <base/trace_event/trace_event.h>

#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/scoped_sequence_context_var.hpp>
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

#include <iostream>
#include <memory>
#include <chrono>

namespace backend {

ExampleServer::ExampleServer(
  ServerStartOptions startOptions)
  : tcpEndpoint_{
      ::boost::asio::ip::make_address(startOptions.ip_addr)
      , startOptions.port_num}
    , asioRegistry_{REFERENCED(ioc_)}
    , tcpEntityAllocator_(REFERENCED(asioRegistry_))
    , listener_{
        ioc_
        , EndpointType{tcpEndpoint_}
        , REFERENCED(asioRegistry_)
        // Callback will be called per each connected client
        // to create ECS entity
        , base::BindRepeating(
            &TcpEntityAllocator::allocateTcpEntity
            , base::Unretained(&tcpEntityAllocator_)
          )
      }
    , signals_set_(ioc_, SIGINT, SIGTERM)
    , mainLoopRunner_{
        base::MessageLoop::current()->task_runner()}
    , asio_thread_1("asio_thread_1")
    , asio_thread_2("asio_thread_2")
    , asio_thread_3("asio_thread_3")
    , asio_thread_4("asio_thread_4")
    , periodicAsioTaskRunner_(
        base::ThreadPool::GetInstance()->
          CreateSequencedTaskRunnerWithTraits(
            base::TaskTraits{
              base::TaskPriority::BEST_EFFORT
              , base::MayBlock()
              , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
            }
          ))
    , periodicConsoleTaskRunner_(
        base::ThreadPool::GetInstance()->
          CreateSequencedTaskRunnerWithTraits(
            base::TaskTraits{
              base::TaskPriority::BEST_EFFORT
              , base::MayBlock()
              , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
            }
          ))
    , consoleTerminal_(base::in_place, periodicConsoleTaskRunner_)
    , networkEntityUpdater_(base::in_place, periodicAsioTaskRunner_)
    , periodicValidateUntil_()
    , is_terminating_(false)
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

#if defined(SIGQUIT)
  signals_set_.add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  signals_set_.async_wait(
    [this]
    (boost::system::error_code const&, int)
    {
      LOG_CALL(DVLOG(99));

      DVLOG(9)
        << "got stop signal";

      {
        DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_mainLoopRunner_);
        DCHECK(mainLoopRunner_);
        (mainLoopRunner_)->PostTask(FROM_HERE
          , base::BindRepeating(
              &ExampleServer::doQuit
              , base::Unretained(this)));
      }
    }
  );
}

void ExampleServer::doQuit()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_mainLoopRunner_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_is_terminating_);

  {
    if(is_terminating_.load())
    {
      LOG(WARNING)
        << "Unable to terminate application twice";
      return;
    }

    is_terminating_.store(true);
  }

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
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &base::Optional<ConsoleTerminalOnSequence>::reset
        , base::Unretained(&consoleTerminal_)
    )
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &base::Optional<NetworkEntityUpdaterOnSequence>::reset
        , base::Unretained(&networkEntityUpdater_)
    )
  )
  /// \todo use |SequenceLocalContext| to store ECS::SimulationRegistry
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

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return base::Promises::All(FROM_HERE
    /// \todo add more acceptors, like WebRTC
    , listener_.stopAcceptorAsync());
}

void ExampleServer::runLoop() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicAsioTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicConsoleTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_mainLoopRunner_);

  DCHECK_RUN_ON(&sequence_checker_);

  prepareBeforeRunLoop();

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

  // Must be resolved when all resources required by `run_loop_.Run()`
  // are freed.
  VoidPromise promiseRunDone
    = VoidPromise::CreateResolved(FROM_HERE);

  {
    // blocking construction of object in sequence-local-storage
    {
      // Append promise to chain as nested promise.
      promiseRunDone =
        promiseRunDone
        .ThenOn(periodicConsoleTaskRunner_
          , FROM_HERE
          , consoleTerminal_->promiseDeletion()
          , /*nestedPromise*/ true
        );

      ConsoleTerminalOnSequence::VoidPromise emplaceDonePromise
        = consoleTerminal_->promiseEmplaceAndStart(FROM_HERE
            , "PeriodicConsoleExecutor" // debug name
            , REFERENCED(periodicConsoleTaskRunner_)
            , base::BindRepeating(
               &ExampleServer::handleConsoleInput
               , base::Unretained(this)
             )
          );

      /// \note Will block current thread for unspecified time.
      base::waitForPromiseResolve(FROM_HERE, emplaceDonePromise);
    }

    // blocking construction of object in sequence-local-storage
    {
      // Append promise to chain as nested promise.
      promiseRunDone =
        promiseRunDone
        .ThenOn(periodicAsioTaskRunner_
          , FROM_HERE
          , networkEntityUpdater_->promiseDeletion()
          , /*nestedPromise*/ true
        );

      NetworkEntityUpdaterOnSequence::VoidPromise emplaceDonePromise
        = networkEntityUpdater_->promiseEmplaceAndStart(FROM_HERE
            , "PeriodicConsoleExecutor" // debug name
            , REFERENCED(periodicAsioTaskRunner_)
            , REFERENCED(asioRegistry_)
            , REFERENCED(ioc_)
          );

      /// \note Will block current thread for unspecified time.
      base::waitForPromiseResolve(FROM_HERE, emplaceDonePromise);
    }

    run_loop_.Run();
  }

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

  // must be reset before `run_loop_.QuitClosure()`
  DCHECK(!consoleTerminal_);

  // must be reset before `run_loop_.QuitClosure()`
  DCHECK(!networkEntityUpdater_);

  /// \note Will block current thread for unspecified time.
  base::waitForPromiseResolve(FROM_HERE, promiseRunDone);
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

void ExampleServer::closeNetworkResources() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicValidateUntil_);

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_.stopped()); // io_context::stopped is thread-safe

  // redirect task to strand
  ::boost::asio::post(
    asioRegistry_.asioStrand()
    , std::bind(
      []
      (
        ECS::AsioRegistry& asioRegistry)
      {
        LOG_CALL(DVLOG(99));

        DCHECK(asioRegistry.running_in_this_thread());

        /// \note it is not ordinary ECS component,
        /// it is stored in entity context (not in ECS registry)
        using WsChannelComponent
          = base::Optional<::flexnet::ws::WsChannel>;

        auto ecsView
          = asioRegistry->view<ECS::TcpConnection>(
              entt::exclude<
                // do not process twice
                ECS::ClosingWebsocket
                // entity in destruction
                , ECS::NeedToDestroyTag
                // entity is unused
                , ECS::UnusedTag
              >
            );

        IterateRegistryCb<ECS::ClosingWebsocket> doEofWebsocket
          = base::BindRepeating(
              []
              (ECS::AsioRegistry& asioRegistry
               , ECS::Entity entity
               , ECS::Registry& registry)
        {
          ignore_result(registry);

          using namespace ::flexnet::ws;

          LOG_CALL(DVLOG(99));

          DCHECK(asioRegistry->valid(entity));

          // each entity representing tcp connection
          // must have that component
          ECS::TcpConnection& tcpComponent
            = asioRegistry->get<ECS::TcpConnection>(entity);

          LOG_CALL(DVLOG(99))
            << " for TcpConnection with id: "
            << tcpComponent.debug_id;

          // `ECS::TcpConnection` must be valid
          DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

          WsChannelComponent* wsChannel
            = tcpComponent->try_ctx_var<WsChannelComponent>();

          if(wsChannel) {
            LOG_CALL(DVLOG(99))
              << " scheduled `async_close` for TcpConnection with id: "
              << tcpComponent.debug_id;
            // schedule `async_close` for websocket connections
            wsChannel->value().postTaskOnConnectionStrand(FROM_HERE,
              base::BindOnce(
                &::flexnet::ws::WsChannel::doEof
                , base::Unretained(&wsChannel->value())
              )
            );
          } else {
            DVLOG(99)
              << "websocket connection component not ready,"
                 " nothing to close for TcpConnection with id: "
              << tcpComponent.debug_id;
          }
        }
        , REFERENCED(asioRegistry));

        if(ecsView.empty()) {
          DVLOG(99)
            << "no open websocket connections,"
               " nothing to close";
        } else {
          // execute callback `doEofWebsocket` per each entity,
          // then mark each entity with `ECS::ClosingWebsocket`
          executeAndEmplace<ECS::ClosingWebsocket>(
            doEofWebsocket
            , asioRegistry
            , ecsView);
        }
      }
      , REFERENCED(asioRegistry_)
    )
  );
}

void ExampleServer::validateAndFreeNetworkResources(
  base::RepeatingClosure resolveCallback) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicValidateUntil_);

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  VLOG(9)
    << "waiting for cleanup of asio registry...";

  /// \note you can not use `::boost::asio::post`
  /// if `ioc->stopped()`
  {
    DCHECK(!ioc_.stopped()); // io_context::stopped is thread-safe
  }

  // send async-close for each connection (on app termination)
  /// \note we periodically call `close` for network entities
  /// because some entities may be not fully created i.e.
  /// we wait for scheduled tasks that will fully create entities.
  closeNetworkResources();

  // redirect task to strand
  ::boost::asio::post(
    asioRegistry_.asioStrand()
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
          // will stop periodic check and resolve promise
          resolveCallback.Run();
        } else {
          DVLOG(9)
            << "registry is NOT empty with size:"
            << asioRegistry->size();
        }
      }
      , REFERENCED(asioRegistry_)
      , COPIED(resolveCallback)
    )
  );
}

ExampleServer::VoidPromise
  ExampleServer::promiseNetworkResourcesFreed() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);
  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_periodicValidateUntil_);

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_.stopped()); // io_context::stopped is thread-safe

  // Will check periodically if `asioRegistry->empty()` and if true,
  // than promise will be resolved.
  // Periodic task will be redirected to `asio_registry.asioStrand()`.
  basis::PeriodicValidateUntil::ValidationTaskType validationTask
    = base::BindRepeating(
        &ExampleServer::validateAndFreeNetworkResources
        , base::Unretained(this)
    );

  return periodicValidateUntil_.runPromise(FROM_HERE
    , basis::EndingTimeout{
        /// \todo make configurable
        base::TimeDelta::FromSeconds(15)} // debug-only expiration time
    , basis::PeriodicCheckUntil::CheckPeriod{
        /// \todo make configurable
        base::TimeDelta::FromSeconds(1)}
      // debug-only error
    , "Destruction of allocated connections hanged."
      "ECS registry must become empty after some time (during app termination)."
      "Not empty registry indicates bugs that need to be reported."
    , base::rvalue_cast(validationTask)
  )
  .ThenHere(
    FROM_HERE
    , base::BindOnce(
      []
      ()
      {
        VLOG(9)
          << "finished cleanup of network entities";
      }
    )
  );
}

ExampleServer::VoidPromise ExampleServer::configureAndRunAcceptor() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_mainLoopRunner_);

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return listener_.configureAndRun()
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      [
      ](
      ){
        VLOG(9)
          << "websocket listener is running";
      }
  ));
}

void ExampleServer::stopIOContext() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  {
    VLOG(1)
      << "stopping io context";

    // Stop the `io_context`. This will cause `io_context.run()`
    // to return immediately, eventually destroying the
    // io_context and any remaining handlers in it.
    ioc_.stop(); // io_context::stop is thread-safe
    DCHECK(ioc_.stopped()); // io_context::stopped is thread-safe
  }
}

ExampleServer::~ExampleServer()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_ioc_);

  DCHECK_RUN_ON(&sequence_checker_);

  DCHECK(ioc_.stopped()); // io_context::stopped is thread-safe
}

void ExampleServer::handleConsoleInput(const std::string& line)
{
  LOG_CALL(DVLOG(99));

  if (line == "stop")
  {
    DVLOG(9)
      << "got `stop` console command";

    DCHECK_CUSTOM_THREAD_GUARD_SCOPE(guard_mainLoopRunner_);
    DCHECK(mainLoopRunner_);
    (mainLoopRunner_)->PostTask(FROM_HERE
      , base::BindRepeating(
          &backend::ExampleServer::doQuit
          , base::Unretained(this)));
  }
}

} // namespace backend
