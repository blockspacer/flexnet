#include "main_plugin_logic.hpp" // IWYU pragma: associated
#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

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

#include <flexnet/websocket/ws_channel.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

namespace plugin {
namespace tcp_server {

MainPluginLogic::MainPluginLogic(
  const MainPluginInterface* pluginInterface)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , configuration_{REFERENCED(
      pluginInterface->metadata()->configuration())}
  , mainLoopRegistry_(
      ::backend::MainLoopRegistry::GetInstance())
  , mainLoopRunner_{
      base::MessageLoop::current()->task_runner()}
  , ioc_{REFERENCED(
      mainLoopRegistry_->registry()
        .ctx<::boost::asio::io_context>())}
  , tcpEndpoint_{
    ::boost::asio::ip::make_address(ipAddr())
    , /// \note Crash if out of range.
      base::checked_cast<unsigned short>(portNum())}
  , asioRegistry_{
      REFERENCED(mainLoopRegistry_->registry()
        .ctx<ECS::AsioRegistry>())}
  , tcpEntityAllocator_(REFERENCED(*asioRegistry_))
  , listener_{
      *ioc_
      , EndpointType{tcpEndpoint_}
      , REFERENCED(*asioRegistry_)
      // Callback will be called per each connected client
      // to create ECS entity
      , base::BindRepeating(
          &::backend::TcpEntityAllocator::allocateTcpEntity
          , base::Unretained(&tcpEntityAllocator_)
        )
    }
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

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(mainLoopRunner_));

  return VoidPromise::CreateResolved(FROM_HERE)
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        &MainPluginLogic::startAcceptors
        , base::Unretained(this)
    )
  );
}

MainPluginLogic::VoidPromise
  MainPluginLogic::unload()
{
  DCHECK_RUN_ON(&sequence_checker_);

  TRACE_EVENT0("headless", "plugin::MainPluginLogic::unload()");

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(mainLoopRunner_));

  return VoidPromise::CreateResolved(FROM_HERE)
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      // Stops creation of new connections.
      /// \note Existing connections may be in `constructing`
      /// state and they can be fully created at arbitrary
      /// (not known beforehand) time due to
      /// asynchronous design of task scheduler.
      &::flexnet::ws::Listener::stopAcceptorAsync
      , base::Unretained(&listener_)
    )
    , base::IsNestedPromise{true}
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
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      // async-wait for destruction of existing connections
      &MainPluginLogic::promiseNetworkResourcesFreed
      , base::Unretained(this)
    )
    , base::IsNestedPromise{true}
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      // stop io context
      /// \note you can not use `::boost::asio::post`
      /// if `ioc_->stopped()`
      /// i.e. can not use strand of registry e.t.c.
      &MainPluginLogic::stopIOContext
      , base::Unretained(this)
    )
  );
}

void MainPluginLogic::startAcceptors() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::PostPromise(FROM_HERE
    /// \note delayed execution:
    /// will be executed only when |run_loop| is running
    , base::MessageLoop::current()->task_runner().get()
    , base::BindOnce(
        &MainPluginLogic::configureAndRunAcceptor
        , base::Unretained(this)
    )
    , base::IsNestedPromise{true}
  );
}

void MainPluginLogic::closeNetworkResources() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(asioRegistry_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(ioc_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicValidateUntil_));

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_->stopped()); // io_context::stopped is thread-safe

  // redirect task to strand
  ::boost::asio::post(
    asioRegistry_->asioStrand()
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
                // entity in destruction
                ECS::NeedToDestroyTag
                // entity is unused
                , ECS::UnusedTag
              >
            );

        base::RepeatingCallback<void(ECS::Entity, ECS::Registry&)> doEofWebsocket
          = base::BindRepeating(
              []
              (ECS::AsioRegistry& asioRegistry
               , ECS::Entity entity
               , ECS::Registry& registry)
        {
          ignore_result(registry);

          using namespace ::flexnet::ws;

          LOG_CALL(DVLOG(99));

          DCHECK(asioRegistry.running_in_this_thread());

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
                //, base::Unretained(&wsChannel->value())
                , wsChannel->value().weakSelf()
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
          ecsView
          .each(
            [&doEofWebsocket, &asioRegistry]
            (const auto& entity
             , const auto& component)
          {
            doEofWebsocket.Run(entity, (*asioRegistry));
          });
        }
      }
      , REFERENCED(*asioRegistry_)
    )
  );
}

void MainPluginLogic::validateAndFreeNetworkResources(
  base::RepeatingClosure resolveCallback) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(asioRegistry_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(ioc_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicValidateUntil_));

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  VLOG(9)
    << "waiting for cleanup of asio registry...";

  /// \note you can not use `::boost::asio::post`
  /// if `ioc->stopped()`
  {
    DCHECK(!ioc_->stopped()); // io_context::stopped is thread-safe
  }

  // send async-close for each connection (on app termination)
  /// \note we periodically call `close` for network entities
  /// because some entities may be not fully created i.e.
  /// we wait for scheduled tasks that will fully create entities.
  closeNetworkResources();

  // redirect task to strand
  ::boost::asio::post(
    asioRegistry_->asioStrand()
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
      , REFERENCED(*asioRegistry_)
      , COPIED(resolveCallback)
    )
  );
}

MainPluginLogic::VoidPromise
  MainPluginLogic::promiseNetworkResourcesFreed() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(ioc_));
  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(periodicValidateUntil_));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_->stopped()); // io_context::stopped is thread-safe

  // Will check periodically if `asioRegistry->empty()` and if true,
  // than promise will be resolved.
  // Periodic task will be redirected to `asio_registry.asioStrand()`.
  basis::PeriodicValidateUntil::ValidationTaskType validationTask
    = base::BindRepeating(
        &MainPluginLogic::validateAndFreeNetworkResources
        , base::Unretained(this)
    );

  return periodicValidateUntil_.runPromise(FROM_HERE
    , basis::EndingTimeout{
        base::TimeDelta::FromMilliseconds(
          quitDetectionDebugTimeoutMillisec())} // debug-only expiration time
    , basis::PeriodicCheckUntil::CheckPeriod{
        base::TimeDelta::FromMilliseconds(
          quitDetectionFreqMillisec())}
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

MainPluginLogic::VoidPromise
  MainPluginLogic::configureAndRunAcceptor() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(mainLoopRunner_));

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

void MainPluginLogic::stopIOContext() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(ioc_));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  {
    VLOG(1)
      << "stopping io context";

    // Stop the `io_context`. This will cause `io_context.run()`
    // to return immediately, eventually destroying the
    // io_context and any remaining handlers in it.
    ioc_->stop(); // io_context::stop is thread-safe
    DCHECK(ioc_->stopped()); // io_context::stopped is thread-safe
  }
}

std::string MainPluginLogic::ipAddr() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  std::string ipAddr
    = kDefaultIpAddr;

  if(configuration_->hasValue(kConfIpAddr))
  {
    ipAddr =
      configuration_->value(kConfIpAddr);
  }

  return ipAddr;
}

int MainPluginLogic::portNum() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int portNum
    = kDefaultPortNum;

  if(configuration_->hasValue(kConfPortNum))
  {
    base::StringToInt(
      configuration_->value(kConfPortNum)
      , &portNum);
  }

  return portNum;
}

int MainPluginLogic::quitDetectionFreqMillisec() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int quitDetectionFreqMillisec
    = kDefaultQuitDetectionFreqMillisec;

  if(configuration_->hasValue(kConfQuitDetectionFreqMillisec))
  {
    base::StringToInt(
      configuration_->value(kConfQuitDetectionFreqMillisec)
      , &quitDetectionFreqMillisec);
  }

  return quitDetectionFreqMillisec;
}

int MainPluginLogic::quitDetectionDebugTimeoutMillisec() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int quitDetectionDebugTimeoutMillisec
    = kDefaultQuitDetectionDebugTimeoutMillisec;

  if(configuration_->hasValue(kConfQuitDetectionDebugTimeoutMillisec))
  {
    base::StringToInt(
      configuration_->value(kConfQuitDetectionDebugTimeoutMillisec)
      , &quitDetectionDebugTimeoutMillisec);
  }

  return quitDetectionDebugTimeoutMillisec;
}

} // namespace tcp_server
} // namespace plugin