#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/util/macros.hpp>
#include <flexnet/util/move_only.hpp>
#include <flexnet/util/unowned_ptr.hpp>
#include <flexnet/util/unowned_ref.hpp>
#include <flexnet/util/promise_collection.hpp>

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

#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

namespace boost {
#ifdef BOOST_NO_EXCEPTIONS
// see https://stackoverflow.com/a/33691561
void throw_exception(const std::exception& ex)
{
  NOTREACHED()
    << "boost thrown exception: "
    << ex.what();
  /// \note application will exit, without returning.
  exit(0);
}
#endif // BOOST_NO_EXCEPTIONS
} // namespace boost

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

  return base::nullopt;
}

class ExampleServer
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  /// \note Lifetime of async callbacks
  /// must be managed externally.
  /// API user can free |DetectChannel| only if
  /// all its callbacks finished (or failed to schedule).
  /// i.e. API user must wait for destruction promise
  using DetectorDestructionResolver
    = base::ManualPromiseResolver<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  using VoidPromiseContainer
    = util::PromiseCollection<void, base::NoReject>;

  using DetectorDestructionResolversContainer
    = std::map<http::DetectChannel*, DetectorDestructionResolver>;

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
  VoidPromise promiseDestructionOfConnections();

  void stopIOContext();

  void addBeforeStopPromise(
    SHARED_LIFETIME(VoidPromise) promise);

  void removeBeforeStopPromise(
    SHARED_LIFETIME(VoidPromise) promise);

 private:
  void onAccepted(
    util::UnownedPtr<ws::Listener>&& listenerWrapper
    , util::UnownedRef<ws::Listener::ErrorCode> ec
    , util::UnownedRef<ws::Listener::SocketType> socket
    , util::UnownedPtr<ws::Listener::StrandType> perConnectionStrand
    , util::ScopedCleanup& scopedDeallocateStrand);

  void onDetectorDetected(
    base::OnceClosure&& doneClosure
    , util::UnownedPtr<http::DetectChannel>&& detectChannelWrapper
    , util::MoveOnly<const http::DetectChannel::ErrorCode>&& ecWrapper
    , util::MoveOnly<const bool>&& handshakeResultWrapper
    , http::DetectChannel::StreamType&& stream
    , http::DetectChannel::MessageBufferType&& buffer);

  // called from destructor of detector,
  // on any thread
  NOT_THREAD_SAFE_FUNCTION()
  void onDetectorDestruct(
    util::UnownedPtr<http::DetectChannel>&& detectChannelWrapper);

 private:
  // maps detector with promise
  // (promise will be resolved on detector destruction)
  DetectorDestructionResolversContainer detector_destruction_resolvers_
    LIVES_ON(sequence_checker_);

  void createAndStoreDetectorDestructionResolver(
    http::DetectChannel* detectChannelPtr);

  void removeAndResolveDetectorDestructionResolver(
    http::DetectChannel* detectChannelPtr);

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
  base::RunLoop run_loop_{};

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  GLOBAL_THREAD_SAFE_LIFETIME()
  boost::asio::signal_set signals_set_{
    ioc_ /// \note will not handle signals if ioc stopped
    , SIGINT
    , SIGTERM
  };

  // all promises that must be resolved before server termination
  VoidPromiseContainer beforeStopPromises_
    LIVES_ON(sequence_checker_);

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
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  ws::Listener::AllocateStrandCallback allocateStrandCallback
    = base::BindRepeating([
      ](
        util::UnownedRef<ws::Listener::StrandType*> strand
        , util::UnownedRef<ws::Listener::IoContext> ioc
        , util::UnownedPtr<ws::Listener>&& listenerWrapper
      )
        -> bool
      {
        LOG_CALL(VLOG(9));

        ignore_result(listenerWrapper);

        /// \note can be replaced with memory pool
        /// to increase performance
        NEW_NO_THROW(FROM_HERE,
          strand.Ref() // lhs of assignment
          , ws::Listener::StrandType(ioc.Ref()) // rhs of assignment
          , LOG(ERROR) // log allocation failure
        );

        return strand.Ref() != nullptr;
      });

  ws::Listener::DeallocateStrandCallback deallocateStrandCallback
    = base::BindRepeating([
      ](
        ws::Listener::StrandType*&& strand
        , util::UnownedPtr<ws::Listener>&& listenerWrapper
      )
        -> bool
      {
        LOG_CALL(VLOG(9));

        ignore_result(listenerWrapper);

        /// \note can be replaced with memory pool
        /// to increase performance
        DELETE_NOT_ARRAY_AND_NULLIFY(FROM_HERE,
          strand);

        return true;
      });

  listener_
    = std::make_unique<ws::Listener>(
        util::UnownedPtr<ws::Listener::IoContext>(&ioc_)
        , EndpointType{tcpEndpoint_}
        , std::move(allocateStrandCallback)
        , std::move(deallocateStrandCallback)
      );

  listener_->registerAcceptedCallback(
    base::BindRepeating(
      &ExampleServer::onAccepted
      , base::Unretained(this)
    )
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
               }
            })
        )
        // async-wait for destruction of existing connections
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::promiseDestructionOfConnections)
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
        // stop io context
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              NESTED_PROMISE(&ExampleServer::stopIOContext)
              , base::Unretained(this)
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

void ExampleServer::onAccepted(
  util::UnownedPtr<ws::Listener>&& listenerWrapper
  , util::UnownedRef<ws::Listener::ErrorCode> ec
  , util::UnownedRef<ws::Listener::SocketType> socket
  , util::UnownedPtr<ws::Listener::StrandType> perConnectionStrand
  , util::ScopedCleanup& scopedDeallocateStrand)
{
  LOG_CALL(VLOG(9));

  ignore_result(listenerWrapper);

  // |scopedDeallocateStrand| can be used to control
  // lifetime of |perConnectionStrand|
  ignore_result(scopedDeallocateStrand);

  DCHECK(perConnectionStrand
    && perConnectionStrand->running_in_this_thread());

  // Handle the error, if any
  if (ec.Ref())
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << ec->message();
    return;
  }

  LOG(INFO)
    << "Listener accepted new connection";

  /// \todo replace unique_ptr with entt registry (pool)
  std::unique_ptr<http::DetectChannel> detectChannel
    = std::make_unique<http::DetectChannel>(
        std::move(socket.Ref())
    );

  // we will |std::move(detectChannel)|, so cache pointer to object
  http::DetectChannel* detectChannelPtr
    = detectChannel.get();

  base::OnceClosure scheduleDeleteDetectorClosure
    = base::BindOnce(
        [
        ](
          std::unique_ptr<http::DetectChannel>&& detectChannel
          , scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner
        ){
          LOG_CALL(VLOG(9));
          DCHECK(detectChannel);
          DCHECK(mainLoopRunner);
          mainLoopRunner->DeleteSoon(FROM_HERE, std::move(detectChannel));
        }
        , base::Passed(std::move(detectChannel))
        , mainLoopRunner_
      );

  DCHECK(detectChannelPtr);
  detectChannelPtr->registerDetectedCallback(
    base::BindOnce(
        &ExampleServer::onDetectorDetected
        , base::Unretained(this)
        , base::Passed(std::move(scheduleDeleteDetectorClosure))
    )
  );

  // |scheduleDeleteDetectorClosure| will destroy |detectChannelPtr|
  // and |detectChannelPtr| will call |onDetectorDestruct|
  DCHECK(detectChannelPtr);
  detectChannelPtr->registerDestructCallback(
    base::BindOnce(
        &ExampleServer::onDetectorDestruct
        , base::Unretained(this)
    )
  );

  base::OnceClosure runDetectorClosure
    = base::BindOnce(
        &http::DetectChannel::runDetector
        , UNOWNED_LIFETIME(base::Unretained(detectChannelPtr))
        , std::chrono::seconds(30) // expire timeout
      );

  base::PostPromise(FROM_HERE
    , UNOWNED_LIFETIME(mainLoopRunner_.get())
    , base::BindOnce(
        &ExampleServer::createAndStoreDetectorDestructionResolver
        , base::Unretained(this)
        , COPIED(detectChannelPtr))
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      NESTED_PROMISE(&base::PostPromiseOnAsioExecutor<
        base::OnceClosure
      >)
      , FROM_HERE
      , CONST_REFERENCED(detectChannelPtr->perConnectionStrand())
      /// \note manage lifetime of |perConnectionStrand|
      , std::move(runDetectorClosure)
    ) // BindOnce
  );
}

NOT_THREAD_SAFE_FUNCTION()
void ExampleServer::onDetectorDestruct(
  util::UnownedPtr<http::DetectChannel>&& detectChannelWrapper)
{
  LOG_CALL(VLOG(9));

  ignore_result(
    base::PostPromise(FROM_HERE
      , UNOWNED_LIFETIME(mainLoopRunner_.get())
      , base::BindOnce(
          &ExampleServer::removeAndResolveDetectorDestructionResolver
          , base::Unretained(this)
          , COPIED(detectChannelWrapper.Get()))
    )
  );
}

void ExampleServer::onDetectorDetected(
  base::OnceClosure&& doneClosure
  , util::UnownedPtr<http::DetectChannel>&& detectChannelWrapper
  , util::MoveOnly<const http::DetectChannel::ErrorCode>&& ecWrapper
  , util::MoveOnly<const bool>&& handshakeResultWrapper
  , http::DetectChannel::StreamType&& stream
  , http::DetectChannel::MessageBufferType&& buffer)
{
  LOG_CALL(VLOG(9));

  const http::DetectChannel::ErrorCode& ec
    = ecWrapper.TakeConst();

  util::ScopedCleanup scopedDoneClosure{[
      &doneClosure
    ](
    ){
      // reset |DetectChannel| (we do not need it anymore)
      std::move(doneClosure).Run();
    }
  };

  DCHECK(detectChannelWrapper
    && detectChannelWrapper->isDetectingInThisThread());

  // Handle the error, if any
  if (ec)
  {
    LOG(ERROR)
      << "Handshake failed for new connection with error: "
      << ec.message();
    return;
  }

  const bool& handshakeResult
    = handshakeResultWrapper.TakeConst();

  if(handshakeResult) {
    LOG(INFO)
      << "Completed secure handshake of new connection";
  } else {
    LOG(INFO)
      << "Completed NOT secure handshake of new connection";
  }

  /// \todo: create channel here
}

void ExampleServer::runLoop()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

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
            /// \note loops forever and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();
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
            /// \note loops forever and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();
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
            /// \note loops forever and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();
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
            /// \note loops forever and
            /// blocks |task_runner->PostTask| for that thread!
            ioc.run();
          }
          , REFERENCED(ioc_)
      )
    );
    asio_thread_4.WaitUntilThreadStarted();
    DCHECK(asio_thread_4.IsRunning());
  }

  run_loop_.Run();

  asio_thread_1.Stop();
  DCHECK(!asio_thread_1.IsRunning());

  asio_thread_2.Stop();
  DCHECK(!asio_thread_2.IsRunning());

  asio_thread_3.Stop();
  DCHECK(!asio_thread_3.IsRunning());

  asio_thread_4.Stop();
  DCHECK(!asio_thread_4.IsRunning());
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

ExampleServer::VoidPromise ExampleServer::promiseDestructionOfConnections()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

#if DCHECK_IS_ON()
  removeBit(beforeStopPromises_.permissions
    , VoidPromiseContainer::Permissions::Addable);
#endif // DCHECK_IS_ON()

  return beforeStopPromises_.All(FROM_HERE);
}

void ExampleServer::stopIOContext()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(!listener_);

  // Stop the io_context. This will cause run()
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  ALWAYS_THREAD_SAFE(ioc_.stop());
  DCHECK(ALWAYS_THREAD_SAFE(ioc_.stopped()));
}

void ExampleServer::removeAndResolveDetectorDestructionResolver(
  http::DetectChannel* detectChannelPtr)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(detectChannelPtr);

  DetectorDestructionResolversContainer::iterator it
    = detector_destruction_resolvers_.find(detectChannelPtr);

  DCHECK(it != detector_destruction_resolvers_.end());

  it->second.Resolve();

  detector_destruction_resolvers_.erase(it
    , detector_destruction_resolvers_.end());
}

void ExampleServer::createAndStoreDetectorDestructionResolver(
  http::DetectChannel* detectChannelPtr)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(detectChannelPtr);

  // do not add same detector twice
  DCHECK(detector_destruction_resolvers_.find(detectChannelPtr)
    == detector_destruction_resolvers_.end());

  std::pair<DetectorDestructionResolversContainer::iterator, bool>
    emplacedResolver
      = detector_destruction_resolvers_.emplace(
          detectChannelPtr
          , FROM_HERE);

  DCHECK(emplacedResolver.second);

  DetectorDestructionResolver& destruction_resolver
    = emplacedResolver.first->second;

  /// \note resolver will be moved out, so we cache promise
  VoidPromise destruction_promise
    = SHARED_LIFETIME(destruction_resolver.promise());

  /// \note tasks below will be scheduled after channel destruction
  /// i.e. will async-wait for destruction promise
  destruction_promise
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        // destroyed connections do not prevent server termination
        &ExampleServer::removeBeforeStopPromise
        , base::Unretained(this)
        , SHARED_LIFETIME(destruction_promise)
      )
  );

  addBeforeStopPromise(COPIED(destruction_promise));
}

void ExampleServer::addBeforeStopPromise(
  SHARED_LIFETIME(VoidPromise) promise)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  beforeStopPromises_.add(promise);
}

void ExampleServer::removeBeforeStopPromise(
  SHARED_LIFETIME(VoidPromise) boundPromise)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  beforeStopPromises_.remove(boundPromise);
}

ExampleServer::~ExampleServer()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(
    ALWAYS_THREAD_SAFE(ioc_.stopped())
    && ASSUME_THREAD_SAFE_BECAUSE(ioc_.stopped())
        beforeStopPromises_.empty()
    && ASSUME_THREAD_SAFE_BECAUSE(ioc_.stopped())
        detector_destruction_resolvers_.empty()
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
