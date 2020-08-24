#include <flexnet/ECS/asio_registry.hpp>

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

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

#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>

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

} // namespace

// Create ECS groups
// Groups offer a faster alternative to multi component views.
// See for details:
// https://github.com/skypjack/entt/wiki/Crash-Course:-entity-component-system#groups
void setEnttGroups()
{
  /// \todo
  //SCOPED_UMA_HISTOGRAM_TIMER( /// \note measures up to 10 seconds
  //  "Startup." + FROM_HERE.ToString());

  ECS::GlobalContext* globals
    = ECS::GlobalContext::GetInstance();
  DCHECK(globals);

  ECS::SimulationRegistry& enttManager
    = globals->ctx_unlocked<ECS::SimulationRegistry>(FROM_HERE);

  ECS::Registry& registry
    = enttManager.ref_registry(FROM_HERE);
}

namespace ECS {

void updateCleanupSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry.ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<ECS::NeedToDestroyTag>();

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " cleaning up entities, total number: "
      << registry_group.size();
  }
  registry_group
    .each(
      [&registry]
      (const Entity& entity
       , const ECS::NeedToDestroyTag& dead_tag)
      mutable
    {
      DCHECK(registry.valid(entity));
    });
#endif // NDEBUG

  registry.destroy(
    registry_group.begin()
    , registry_group.end());
}

} // namespace ECS

namespace ECS {

static std::unique_ptr<basis::PeriodicTaskExecutor> waitCleanupExecutor
  = nullptr;

static scoped_refptr<base::SequencedTaskRunner> waitCleanupRunner
  = nullptr;

base::Promise<void, base::NoReject> waitCleanupConnectionResources(
  ECS::AsioRegistry& asio_registry)
{
  base::ManualPromiseResolver<
      void, base::NoReject
    >
    promiseResolver(FROM_HERE);

  DCHECK(waitCleanupRunner
    && waitCleanupRunner->RunsTasksInCurrentSequence());

  waitCleanupExecutor = std::make_unique<basis::PeriodicTaskExecutor>(
    waitCleanupRunner
    , base::BindRepeating(
        [
        ](
          ECS::AsioRegistry& asio_registry
          , base::OnceClosure resolveCallback
        ){
          LOG(INFO)
            << "waiting for cleanup of asio registry...";

          ::boost::asio::post(
            asio_registry.ref_strand(FROM_HERE)
            , ::boost::beast::bind_front_handler([
              ](
                ECS::AsioRegistry& asio_registry
                , base::OnceClosure resolveCallback
              ){
                DCHECK(
                  asio_registry.ref_strand(FROM_HERE).running_in_this_thread());
                ECS::Registry& registry
                  /// \note take care of thread-safety
                  = asio_registry
                  .ref_registry_unsafe(FROM_HERE);
                if(registry.empty()) {
                  DVLOG(9)
                    << "registry is empty";
                  std::move(resolveCallback).Run();
                  /// \note will stop periodic timer on scope exit
                  DCHECK(waitCleanupRunner);
                  DCHECK(waitCleanupExecutor);
                  waitCleanupRunner->DeleteSoon(FROM_HERE
                    , std::move(waitCleanupExecutor));
                } else {
                  DVLOG(9)
                    << "registry is NOT empty";
                }
              }
              , REFERENCED(asio_registry)
              , std::move(resolveCallback)
            )
          );
        }
        , REFERENCED(asio_registry)
        , promiseResolver.GetRepeatingResolveCallback()
    )
  );

  waitCleanupExecutor->startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(500));

  return promiseResolver.promise();
}

} // namespace ECS

namespace ECS {

void updateUnusedSystem(
  ECS::AsioRegistry& asio_registry)
{
  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
    .ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<ECS::UnusedTag>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >);

#if !defined(NDEBUG)
  if(registry_group.size()) {
    DVLOG(99)
      << " found unused entities, total number: "
      << registry_group.size();
  }
#endif // NDEBUG

  registry_group
    .each(
      [&registry]
      (const Entity& entity
       , const ECS::UnusedTag& dead_tag)
      mutable
    {
      DCHECK(registry.valid(entity));
      registry.assign<ECS::NeedToDestroyTag>(entity);
    });
}

} // namespace ECS

namespace ECS {

void handleAcceptNewConnectionResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::ws::Listener::AcceptNewConnectionResult& acceptResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
    .ref_registry(FROM_HERE);

  ECS::AsioRegistry::StrandType asioRegistryStrand
    = asio_registry
    .ref_strand(FROM_HERE);

  DCHECK(asioRegistryStrand.running_in_this_thread());

  // Handle the error, if any
  if (acceptResult.ec)
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << acceptResult.ec.message();

    // it is safe to destroy entity now
    if(!registry.has<ECS::UnusedTag>(entity_id)) {
      registry.assign<ECS::UnusedTag>(entity_id);
    }

    return;
  }

  LOG(INFO)
    << "Listener accepted new connection";

  /// \todo replace unique_ptr with entt registry (pool)
  std::unique_ptr<http::DetectChannel> detectChannel
    = std::make_unique<http::DetectChannel>(
        std::move(acceptResult.socket)
        , RAW_REFERENCED(asio_registry)
        , entity_id
    );

  DCHECK(asioRegistryStrand.running_in_this_thread());
  http::DetectChannel* detectChannelPtr
    = registry
      .assign<std::unique_ptr<http::DetectChannel>>(
        entity_id
        , std::move(detectChannel)).get();

  ::boost::asio::post(
    detectChannelPtr->perConnectionStrand()
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        std::move(task).Run();
      }
      , base::BindOnce(
        &http::DetectChannel::runDetector
        , UNOWNED_LIFETIME(base::Unretained(detectChannelPtr))
        // expire timeout for SSL detection
        , std::chrono::seconds(3)
      )
    )
  );
}

CREATE_ECS_TAG(UnusedAcceptResultTag)

void updateNewConnections(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = std::unique_ptr<ws::Listener::AcceptNewConnectionResult>;

  DCHECK(
    asio_registry
    .ref_strand(FROM_HERE)
    .running_in_this_thread());

  ECS::Registry& registry
    = asio_registry.
      ref_registry(FROM_HERE);

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    = registry.view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity is unused
          , ECS::UnusedTag
          // components related to acceptor are unused
          , ECS::UnusedAcceptResultTag
        >
      );

  registry_group
    .each(
      [&registry, &asio_registry]
      (const Entity& entity
       , const view_component& component)
      mutable
    {
      DCHECK(registry.valid(entity));

      handleAcceptNewConnectionResult(asio_registry, entity, *component);

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry.has<ECS::UnusedAcceptResultTag>(entity)) {
        registry.assign<ECS::UnusedAcceptResultTag>(entity);
      }
    });
}

} // namespace ECS

namespace ECS {

void handleSSLDetectResult(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , flexnet::http::DetectChannel::SSLDetectResult& detectResult)
{
  using namespace ::flexnet::ws;
  using namespace ::flexnet::http;

  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
      .ref_registry(FROM_HERE);

  ECS::AsioRegistry::StrandType asioRegistryStrand
    = asio_registry.ref_strand(FROM_HERE);

  DCHECK(asioRegistryStrand.running_in_this_thread());

  LOG_CALL(VLOG(9));

  auto closeAndReleaseResources
    = [&detectResult, &registry, entity_id]()
  {
    // Send shutdown
    if(detectResult.stream.socket().is_open())
    {
      DVLOG(9) << "shutdown of stream...";
      boost::beast::error_code ec;
      detectResult.stream.socket().shutdown(
        boost::asio::ip::tcp::socket::shutdown_send, ec);
      if (ec) {
        LOG(WARNING)
          << "error during stream shutdown: "
          << ec.message();
      }
    }

    // it is safe to destroy entity now
    if(!registry.has<ECS::UnusedTag>(entity_id)) {
      registry.assign<ECS::UnusedTag>(entity_id);
    }
  };

  // Handle the error, if any
  if (detectResult.ec)
  {
    LOG(ERROR)
      << "Handshake failed for new connection with error: "
      << detectResult.ec.message();

    closeAndReleaseResources();

    DCHECK(!detectResult.stream.socket().is_open());

    return;
  }

  if(detectResult.handshakeResult) {
    LOG(INFO)
      << "Completed secure handshake of new connection";
  } else {
    LOG(INFO)
      << "Completed NOT secure handshake of new connection";
  }

  DCHECK(detectResult.stream.socket().is_open());

  /// \todo: create channel here
  // Create the session and run it
  //std::make_shared<session>(std::move(detectResult.stream.socket()))->run();
  closeAndReleaseResources();
}

CREATE_ECS_TAG(UnusedSSLDetectResultTag)

void updateSSLDetection(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = std::unique_ptr<http::DetectChannel::SSLDetectResult>;

  DCHECK(
    asio_registry.ref_strand(FROM_HERE).running_in_this_thread());

  ECS::Registry& registry
    = asio_registry
      .ref_registry(FROM_HERE);

  auto registry_group
    = registry.view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity is unused
          , ECS::UnusedTag
          // components related to SSL detection are unused
          , ECS::UnusedSSLDetectResultTag
        >
      );

  registry_group
    .each(
      [&registry, &asio_registry]
      (const Entity& entity
       , const view_component& component)
      mutable
    {
      DCHECK(registry.valid(entity));

      handleSSLDetectResult(asio_registry, entity, *component);

      // do not process twice
      // similar to
      // `registry.remove<view_component>(entity);`
      // except avoids extra allocations
      // i.e. can be used with memory pool
      if(!registry.has<ECS::UnusedSSLDetectResultTag>(entity)) {
        registry.assign<ECS::UnusedSSLDetectResultTag>(entity);
      }
    });
}

} // namespace ECS

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
        "ECS::SimulationRegistry");

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

class ExampleServer
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  using EndpointType
    = ws::Listener::EndpointType;

 public:
  ExampleServer();

  ~ExampleServer();

  void runLoop();

  MUST_USE_RETURN_VALUE
  StatusPromise stopAcceptors();

  MUST_USE_RETURN_VALUE
  VoidPromise configureAndRunAcceptor();

  MUST_USE_RETURN_VALUE
  VoidPromise waitNetworkResourcesFreed();

  void stopIOContext();

 private:
  // The io_context is required for all I/O
  GLOBAL_THREAD_SAFE_LIFETIME()
  boost::asio::io_context ioc_{};

  GLOBAL_THREAD_SAFE_LIFETIME()
  const boost::asio::ip::address address_
    = ::boost::asio::ip::make_address("127.0.0.1");

  GLOBAL_THREAD_SAFE_LIFETIME()
  const unsigned short port_
    = 8085;

  GLOBAL_THREAD_SAFE_LIFETIME()
  const EndpointType tcpEndpoint_{address_, port_};

  /// \todo SSL support
  // GLOBAL_THREAD_SAFE_LIFETIME()
  // ::boost::asio::ssl::context ctx_
  //   {::boost::asio::ssl::context::tlsv12};

  GLOBAL_THREAD_SAFE_LIFETIME()
  std::unique_ptr<ws::Listener> listener_;

  GLOBAL_THREAD_SAFE_LIFETIME()
  ECS::AsioRegistry asioRegistry_;

  GLOBAL_THREAD_SAFE_LIFETIME()
  base::RunLoop run_loop_{};

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  GLOBAL_THREAD_SAFE_LIFETIME()
  boost::asio::signal_set signals_set_{
    ioc_ /// \note will not handle signals if ioc stopped
    , SIGINT
    , SIGTERM
  };

  GLOBAL_THREAD_SAFE_LIFETIME()
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    = base::MessageLoop::current()->task_runner();

  GLOBAL_THREAD_SAFE_LIFETIME()
  base::Thread asio_thread_1{"asio_thread_1"};

  GLOBAL_THREAD_SAFE_LIFETIME()
  base::Thread asio_thread_2{"asio_thread_2"};

  GLOBAL_THREAD_SAFE_LIFETIME()
  base::Thread asio_thread_3{"asio_thread_3"};

  GLOBAL_THREAD_SAFE_LIFETIME()
  base::Thread asio_thread_4{"asio_thread_4"};

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

ExampleServer::ExampleServer()
  : asioRegistry_(util::UnownedPtr<ws::Listener::IoContext>(&ioc_))
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  listener_
    = std::make_unique<ws::Listener>(
        util::UnownedPtr<ws::Listener::IoContext>(&ioc_)
        , EndpointType{tcpEndpoint_}
        , RAW_REFERENCED(asioRegistry_)
      );

#if defined(SIGQUIT)
  signals_set_.add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  auto sigQuitCallback
    = [this]
      (boost::system::error_code const&, int)
      {
        LOG_CALL(VLOG(9));

        DCHECK(mainLoopRunner_);

        LOG(INFO)
          << "got stop signal";

        // stop accepting of new connections
        base::PostPromise(FROM_HERE
          , UNOWNED_LIFETIME(mainLoopRunner_.get())
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::stopAcceptors)
              , base::Unretained(this))
        )
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
            [
            ](
              const ::util::Status& stopAcceptorResult
            ){
               LOG_CALL(VLOG(9));

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
              NESTED_PROMISE(&ExampleServer::waitNetworkResourcesFreed)
              , base::Unretained(this)
          )
        )
        // stop io context
        /// \note you can not use `::boost::asio::post`
        /// if `ioc_->stopped()`
        /// i.e. can not use strand of registry e.t.c.
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::stopIOContext)
              , base::Unretained(this)
          )
        )
        // reset |listener_|
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              &std::unique_ptr<ws::Listener>::reset
              , base::Unretained(&listener_)
              , nullptr // reset unique_ptr to nullptr
            )
        )
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              // |GlobalContext| is not thread-safe,
              // so modify it only from one sequence
              &ECS::GlobalContext::unlockModification
              , base::Unretained(ECS::GlobalContext::GetInstance())
            )
        )
        // delete |ECS::SimulationRegistry|
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              &ECS::GlobalContext::unset<ECS::SimulationRegistry>
              , base::Unretained(ECS::GlobalContext::GetInstance())
              , FROM_HERE
            )
        )
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , run_loop_.QuitClosure());
      };

  signals_set_.async_wait(std::move(sigQuitCallback));
}

ExampleServer::StatusPromise ExampleServer::stopAcceptors()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return base::Promises::All(FROM_HERE
    /// \todo add more acceptors
    , listener_->stopAcceptorAsync());
}

void ExampleServer::runLoop()
{
  LOG_CALL(VLOG(9));

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
        [
        ](
          ECS::AsioRegistry& asio_registry
          , boost::asio::io_context& ioc
        ){
          /// \note you can not use `::boost::asio::post`
          /// if `ioc_->stopped()`
          if(ioc.stopped()) {
            LOG(ERROR)
              << "skipping update of registry"
                 " because of stopped io context";
            NOTREACHED();
            return;
          }

          ::boost::asio::post(
            asio_registry.ref_strand(FROM_HERE)
            , ::boost::beast::bind_front_handler([
              ](
                ECS::AsioRegistry& asio_registry
              ){
                DVLOG(9)
                  << "updating asio registry...";

                DCHECK(
                  asio_registry.ref_strand(FROM_HERE).running_in_this_thread());
                ECS::Registry& registry
                  /// \note take care of thread-safety
                  = asio_registry
                    .ref_registry_unsafe(FROM_HERE);
                ECS::updateNewConnections(asio_registry);
                ECS::updateSSLDetection(asio_registry);
                /// \todo cutomizable cleanup period
                ECS::updateUnusedSystem(asio_registry);
                /// \todo cutomizable cleanup period
                ECS::updateCleanupSystem(asio_registry);
              }
              , REFERENCED(asio_registry)
            )
          );
        }
        , REFERENCED(asioRegistry_)
        , REFERENCED(ioc_)
    )
  );

  periodicAsioExecutor.startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(100));

  {
    base::Thread::Options options;
    asio_thread_1.StartWithOptions(options);
    asio_thread_1.task_runner()->PostTask(FROM_HERE
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
}

ExampleServer::VoidPromise ExampleServer::waitNetworkResourcesFreed()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ECS::waitCleanupRunner
    = base::ThreadPool::GetInstance()->
      CreateSequencedTaskRunnerWithTraits(
        base::TaskTraits{
          base::TaskPriority::BEST_EFFORT
          , base::MayBlock()
          , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
        }
      );

  /// \todo crash on timeout
  return base::PostPromise(
    FROM_HERE
    // Post our work to the strand, to prevent data race
    , ECS::waitCleanupRunner.get()
    , base::BindOnce(
        NESTED_PROMISE(&ECS::waitCleanupConnectionResources)
        /// \note take care of thread-safety
        , REFERENCED(asioRegistry_))
    )
    // reset |waitCleanupRunner|
    .ThenOn(base::MessageLoop::current()->task_runner()
      , FROM_HERE
      , base::BindOnce(
          &scoped_refptr<base::SequencedTaskRunner>::reset
          , base::Unretained(&ECS::waitCleanupRunner)
        )
    );
}

ExampleServer::VoidPromise ExampleServer::configureAndRunAcceptor()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return listener_->configureAndRun()
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

void ExampleServer::stopIOContext()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Stop the io_context. This will cause run()
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  ALWAYS_THREAD_SAFE(ioc_.stop());
  DCHECK(ALWAYS_THREAD_SAFE(ioc_.stopped()));
}

ExampleServer::~ExampleServer()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(
    ALWAYS_THREAD_SAFE(ioc_.stopped())
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

  base::PostPromise(FROM_HERE
        /// \note delayed execution:
        /// will be executed only when |run_loop| is running
        , base::MessageLoop::current()->task_runner().get()
        , base::BindOnce(
            NESTED_PROMISE(&ExampleServer::configureAndRunAcceptor)
            , base::Unretained(&exampleServer)
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

  exampleServer.runLoop();

  LOG(INFO)
    << "server is quitting";

  return
    EXIT_SUCCESS;
}
