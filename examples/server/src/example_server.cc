#include "example_server.hpp" // IWYU pragma: associated

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
#include <flexnet/util/lock_with_check.hpp>
#include <flexnet/util/periodic_validate_until.hpp>
#include <flexnet/util/scoped_sequence_context_var.hpp>

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

namespace {

constexpr char kFeatureConsoleTerminalName[]
  = "console_terminal";

const base::Feature kFeatureConsoleTerminal {
  kFeatureConsoleTerminalName, base::FEATURE_DISABLED_BY_DEFAULT
};

#if DCHECK_IS_ON()
// must match type of `entt::type_info<SomeType>::id()`
using entt_type_info_id_type
  = std::uint32_t;

struct DebugComponentIdWithName {
  std::string debugName;
  entt_type_info_id_type id;
};
#endif // DCHECK_IS_ON()

} // namespace

namespace ECS {

// do not schedule `async_close` twice
CREATE_ECS_TAG(ClosingWebsocket)

} // namespace ECS

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
        , REFERENCED(asioRegistry_)
        , base::BindRepeating(
            &ExampleServer::allocateTcpEntity
            , base::Unretained(this)
          )
      }
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

ECS::Entity ExampleServer::allocateTcpEntity() NO_EXCEPTION
{
  using DetectChannel
    = ::flexnet::http::DetectChannel;

  using Listener
    = ::flexnet::ws::Listener;

  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

  DCHECK(asioRegistry_.running_in_this_thread());

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    /// \todo use `entt::group` instead of `entt::view` here
    /// if it will speed things up
    /// i.e. measure perf. and compare results
    = asioRegistry_->view<ECS::TcpConnection, ECS::UnusedTag>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >
      );

  ECS::Entity tcp_entity_id
    = ECS::NULL_ENTITY;

  bool usedCache = false;

  if(registry_group.empty())
  {
    tcp_entity_id = asioRegistry_->create();
    DCHECK(!usedCache);
    DVLOG(99)
      << "allocating new network entity";
    DCHECK(asioRegistry_->valid(tcp_entity_id));
  } else { // if from `cache`
    usedCache = true;

    // reuse any unused entity (if found)
    for(const ECS::Entity& entity_id : registry_group)
    {
      if(!asioRegistry_->valid(entity_id)) {
        // skip invalid entities
        continue;
      }
      tcp_entity_id = entity_id;
      // need only first valid entity
      break;
    }

    /// \note may not find valid entities,
    /// so checks for `ECS::NULL_ENTITY` using `registry.valid`
    if(asioRegistry_->valid(tcp_entity_id)) {
      asioRegistry_->remove<ECS::UnusedTag>(tcp_entity_id);
      DVLOG(99)
        << "using preallocated network entity with id: "
        << asioRegistry_->get<ECS::TcpConnection>(tcp_entity_id).debug_id;

    } else {
      tcp_entity_id = asioRegistry_->create();
      DCHECK(!usedCache);
      DVLOG(99)
        << "allocating new network entity";
    }
  } // else

  DCHECK(asioRegistry_->valid(tcp_entity_id));

  /// \note `tcpComponent.context.empty()` may be false
  /// if unused `tcpComponent` found using `registry.get`
  /// i.e. we do not fully reset `tcpComponent`,
  /// so reset each type stored in context individually.
  ECS::TcpConnection& tcpComponent
    = asioRegistry_->get_or_emplace<ECS::TcpConnection>(
        tcp_entity_id
        , "TcpConnection_" + base::GenerateGUID() // debug name
      );

  if(usedCache)
  {
    /// \todo move `cached entity usage validator` to separate callback
    // Clear cached data.
    // We re-use some old network entity,
    // so need to reset entity state.
    DCHECK(asioRegistry_->valid(tcp_entity_id));

    // sanity check
    CHECK(asioRegistry_->has<
        ECS::TcpConnection
      >(tcp_entity_id));

    // sanity check
    CHECK(!asioRegistry_->has<
        ECS::UnusedTag
      >(tcp_entity_id));

    // sanity check
    CHECK(!asioRegistry_->has<
        ECS::NeedToDestroyTag
      >(tcp_entity_id));

    /// \note not cached, affects performance
    asioRegistry_->remove_if_exists<
      ECS::ClosingWebsocket
    >(tcp_entity_id);

    /// \note not cached, affects performance
    asioRegistry_->remove_if_exists<
      ECS::CloseSocket
    >(tcp_entity_id);

    // required to process `SSLDetectResult` once
    // (i.e. not handle outdated result)
    asioRegistry_->emplace_or_replace<
      ECS::UnusedSSLDetectResultTag
    >(tcp_entity_id);

    // required to process `AcceptResult` once
    // (i.e. not handle outdated result)
    asioRegistry_->emplace_or_replace<
      ECS::UnusedAcceptResultTag
    >(tcp_entity_id);

#if DCHECK_IS_ON()
    /// \note place here only types that can be re-used
    /// i.e. components that can be re-used from cache
    static std::vector<
      DebugComponentIdWithName
    > allowedComponents{
      {
        "TcpConnection"
        , entt::type_info<ECS::TcpConnection>::id()
      }
      , {
        "SSLDetectResult"
        , entt::type_info<base::Optional<DetectChannel::SSLDetectResult>>::id()
      }
      , {
        "UnusedSSLDetectResultTag"
        , entt::type_info<ECS::UnusedSSLDetectResultTag>::id()
      }
      , {
        "AcceptConnectionResult"
        , entt::type_info<base::Optional<Listener::AcceptConnectionResult>>::id()
      }
      , {
        "UnusedAcceptResultTag"
        , entt::type_info<ECS::UnusedAcceptResultTag>::id()
      }
    };

    // visit entity and get the types of components it manages
    asioRegistry_->visit(tcp_entity_id
      , [this, tcp_entity_id]
      (const entt_type_info_id_type type_id)
      {
        const auto it
          = std::find_if(allowedComponents.begin()
            , allowedComponents.end()
            // custom comparator
            , [&type_id]
            (const DebugComponentIdWithName& x)
            {
              DVLOG(99)
                << " x.id = "
                << x.id
                << " type_id ="
                << type_id;
              /// \note compare only by id, without debug name
              return x.id == type_id;
            }
          );

        if(it == allowedComponents.end())
        {
          DLOG(ERROR)
            << " cached entity with id = "
            << tcp_entity_id
            << " is NOT allowed to contain component with id = "
            << type_id;
          NOTREACHED();
        } else {
          DVLOG(99)
            << " cached entity with id = "
            << tcp_entity_id
            << " re-uses component with name ="
            << it->debugName
            << " component with id = "
            << type_id;
        }
      }
    );

    ECS::UnsafeTypeContext& ctx
      = tcpComponent.context;

    std::vector<ECS::UnsafeTypeContext::variable_data>& ctxVars
      = ctx.vars();

    /// \note place here only types that can be re-used
    /// i.e. context vars that can be re-used from cache
    static std::vector<
      DebugComponentIdWithName
    > allowedCtxVars{
      {
        "StrandComponent"
        , entt::type_info<Listener::StrandComponent>::id()
      }
      , {
        "HttpChannel"
        , entt::type_info<base::Optional<::flexnet::http::HttpChannel>>::id()
      }
      , {
        "WsChannel"
        , entt::type_info<base::Optional<::flexnet::ws::WsChannel>>::id()
      }
      , {
        "DetectChannel"
        , entt::type_info<base::Optional<::flexnet::http::DetectChannel>>::id()
      }
    };

    for(auto pos = ctxVars.size(); pos; --pos)
    {
      auto type_id = ctxVars[pos-1].type_id;

      const auto it
        = std::find_if(allowedCtxVars.begin()
          , allowedCtxVars.end()
          // custom comparator
          , [&type_id]
          (const DebugComponentIdWithName& x)
          {
            DVLOG(99)
              << " x.id = "
              << x.id
              << " type_id ="
              << type_id;
            /// \note compare only by id, without debug name
            return x.id == type_id;
          }
        );

      if(it == allowedCtxVars.end())
      {
        DLOG(ERROR)
          << " cached context var with id = "
          << tcp_entity_id
          << " is NOT allowed to contain context var with id = "
          << type_id;
        NOTREACHED();
      } else {
        DVLOG(99)
          << " cached entity with id = "
          << tcp_entity_id
          << " re-uses context var with name ="
          << it->debugName
          << " context var with id = "
          << type_id;
      }
    }
#endif // DCHECK_IS_ON()
  } // if

  return tcp_entity_id;
}

void ExampleServer::hangleQuitSignal()
{
  LOG_CALL(DVLOG(99));

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

void ExampleServer::updateConsoleTerminal() NO_EXCEPTION
{
  DCHECK_CUSTOM_THREAD_GUARD(periodicConsoleTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicConsoleTaskRunner_.get());

  if(ioc_.stopped()) // io_context::stopped is thread-safe
  {
    LOG(WARNING)
      << "skipping update of console terminal"
         " because of stopped io context";

    return;
  }

  std::string line;

  /// \todo getline blocks without timeout, so add timeout like in
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

  if (line == "stop")
  {
    DVLOG(99)
      << "got `stop` console command";

    DCHECK_CUSTOM_THREAD_GUARD(mainLoopRunner_);
    DCHECK(mainLoopRunner_);
    (mainLoopRunner_)->PostTask(FROM_HERE
      , base::BindRepeating(
          &ExampleServer::hangleQuitSignal
          , base::Unretained(this)));
  }
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

void ExampleServer::runLoop() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(periodicAsioTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD(periodicConsoleTaskRunner_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

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

  {
    DCHECK(!periodicConsoleTaskRunner_);
    periodicConsoleTaskRunner_
      = base::ThreadPool::GetInstance()->
         CreateSequencedTaskRunnerWithTraits(
           base::TaskTraits{
             base::TaskPriority::BEST_EFFORT
             , base::MayBlock()
             , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
           }
         );
  }

  // Must be resolved when all resources required by `run_loop_.Run()`
  // are freed.
  VoidPromise promiseRunDone
    = VoidPromise::CreateResolved(FROM_HERE);

  {
    // Create UNIQUE type to store in sequence-local-context
    /// \note initialized, used and destroyed
    /// on `TaskRunner` sequence-local-context
    using ConsoleTerminalUpdater
      = util::StrongAlias<
          class ConsoleTerminalExecutorTag
          /// \note will stop periodic timer on scope exit
          , basis::PeriodicTaskExecutor
        >;
    /// \note Will free stored variable on scope exit.
    basis::ScopedSequenceCtxVar<ConsoleTerminalUpdater> scopedSequencedConsoleExecutor
      (periodicConsoleTaskRunner_);

    {
      VoidPromise emplaceDonePromise
        = scopedSequencedConsoleExecutor.emplace_async(FROM_HERE
              , "PeriodicConsoleExecutor" // debug name
              , base::BindRepeating(
                  &ExampleServer::updateConsoleTerminal
                  , base::Unretained(this))
        )
      .ThenOn(periodicConsoleTaskRunner_
        , FROM_HERE
        , base::BindOnce(
          []
          (
            scoped_refptr<base::SequencedTaskRunner> periodicRunner
            , ConsoleTerminalUpdater* consoleUpdater
          ){
            LOG_CALL(DVLOG(99));

            if(!base::FeatureList::IsEnabled(kFeatureConsoleTerminal))
            {
              DVLOG(99)
                << "console terminal not enabled";
              return;
            }

            /// \todo make period configurable
            (*consoleUpdater)->startPeriodicTimer(
              base::TimeDelta::FromMilliseconds(100));

            DCHECK(periodicRunner->RunsTasksInCurrentSequence());
          }
          , periodicConsoleTaskRunner_
        )
      );
      /// \note Will block current thread for unspecified time.
      base::waitForPromiseResolve(FROM_HERE, emplaceDonePromise);
    }

    // Create UNIQUE type to store in sequence-local-context
    /// \note initialized, used and destroyed
    /// on `TaskRunner` sequence-local-context
    using AsioUpdater
      = util::StrongAlias<
          class PeriodicAsioExecutorTag
          /// \note will stop periodic timer on scope exit
          , basis::PeriodicTaskExecutor
        >;

    /// \note Will free stored variable on scope exit.
    basis::ScopedSequenceCtxVar<AsioUpdater> scopedSequencedAsioExecutor
      (periodicAsioTaskRunner_);

    {
      VoidPromise emplaceDonePromise
        = scopedSequencedAsioExecutor.emplace_async(FROM_HERE
              , "PeriodicAsioExecutor" // debug name
              , base::BindRepeating(
                  /// \note updateAsioRegistry is not thread-safe
                  &ExampleServer::updateAsioRegistry
                  , base::Unretained(this))
        )
      .ThenOn(periodicAsioTaskRunner_
        , FROM_HERE
        , base::BindOnce(
          []
          (
            scoped_refptr<base::SequencedTaskRunner> periodicAsioTaskRunner
            , AsioUpdater* periodicAsioExecutor
          ){
            LOG_CALL(DVLOG(99));

            /// \todo make period configurable
            (*periodicAsioExecutor)->startPeriodicTimer(
              base::TimeDelta::FromMilliseconds(100));

            DCHECK(periodicAsioTaskRunner->RunsTasksInCurrentSequence());
          }
          , periodicAsioTaskRunner_
        )
      );
      /// \note Will block current thread for unspecified time.
      base::waitForPromiseResolve(FROM_HERE, emplaceDonePromise);
    }

    // Append promise to chain as nested promise.
    promiseRunDone =
      promiseRunDone
      .ThenOn(periodicAsioTaskRunner_
        , FROM_HERE
        // Wait for deletion
        // on task runner associated with it
        // otherwise you can get `use-after-free` error
        // on resources bound to periodic callback.
        /// \note We can not just call `TaskRunner_.reset()`
        /// because `TaskRunner_` is shared type
        /// and because created executor prolongs its lifetime.
        , scopedSequencedAsioExecutor.promiseDeletion()
        , /*nestedPromise*/ true
      )
      .ThenOn(periodicConsoleTaskRunner_
        , FROM_HERE
        // Wait for deletion
        // on task runner associated with it
        // otherwise you can get `use-after-free` error
        // on resources bound to periodic callback.
        /// \note We can not just call `TaskRunner_.reset()`
        /// because `TaskRunner_` is shared type
        /// and because created executor prolongs its lifetime.
        , scopedSequencedConsoleExecutor.promiseDeletion()
        , /*nestedPromise*/ true
      );

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

template<typename ComponentType>
using IterateRegistryCb
  = base::RepeatingCallback<
      void(ECS::Entity, ECS::Registry& registry)>;

// Use it if you want to perform some task with entity and then mark entity
// as `already processed` (to avoid processing twice).
//
// ALGORITHM
//
// 1 step:
// Executes provided callback (per each entity in `entities view`).
// 2 step:
// Adds component into each entity in `entities view`
// using provided ECS registry.
template<
  // component to emplace (if not exists)
  typename ComponentType
  // filtered `entities view`
  , typename EntitiesViewType
  // arguments for component construction
  , class... Args
>
static void executeAndEmplace(
  IterateRegistryCb<ComponentType> taskCb
  , ECS::AsioRegistry& asioRegistry
  , EntitiesViewType& ecsView
  , Args&&... args)
{
  LOG_CALL(DVLOG(99));

  DCHECK(asioRegistry.running_in_this_thread());

  ecsView
    .each(
      [&taskCb, &asioRegistry, ... args = std::forward<Args>(args)]
      (const auto& entity
       , const auto& component)
    {
      taskCb.Run(entity, asioRegistry.registry());
    });

  asioRegistry->insert<ComponentType>(
    ecsView.begin()
    , ecsView.end()
    , std::forward<Args>(args)...);

#if DCHECK_IS_ON()
  // sanity check due to bug in old entt versions
  // checks `registry.insert(begin, end)`
  ecsView
    .each(
      [&asioRegistry, ... args = std::forward<Args>(args)]
      (const auto& entity
       , const auto& component)
    {
      // inserted component must exist
      DCHECK(asioRegistry->has<ComponentType>(entity));
    });
#endif // DCHECK_IS_ON()
}

void ExampleServer::closeNetworkResources() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  /// \note you can not use `::boost::asio::post`
  /// if `ioc_->stopped()`
  DCHECK(!ioc_.stopped()); // io_context::stopped is thread-safe

  // redirect task to strand
  ::boost::asio::post(
    asioRegistry_.strand()
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

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK(periodicValidateUntil_.RunsVerifierInCurrentSequence());

  LOG(INFO)
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
    asioRegistry_.strand()
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
