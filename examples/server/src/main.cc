#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/util/macros.hpp>

#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
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

using namespace flexnet;

namespace {

static const base::FilePath::CharType kIcuDataFileName[]
  = FILE_PATH_LITERAL(R"raw(./resources/icu/optimal/icudt64l.dat)raw");

static const base::FilePath::CharType kTraceReportFileName[]
  = FILE_PATH_LITERAL(R"raw(trace_report.json)raw");

// Check to see that we are being called on only one thread.
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
[[nodiscard]] /* do not ignore return value */
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

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  // Define a total order based on the |task_runner| affinity, so that MDPs
  // belonging to the same SequencedTaskRunner are adjacent in the set.
  struct VoidPromiseComparator {
    bool operator()(const VoidPromise& a,
                    const VoidPromise& b) const;
  };

  using VoidPromiseContainer =
      std::set<SHARED_LIFETIME(VoidPromise), VoidPromiseComparator>;

 public:
  ExampleServer();

  ~ExampleServer();

  void runLoop();

  StatusPromise stopAcceptors();

  VoidPromise configureAndRunAcceptor();

  VoidPromise promiseDestructionOfConnections();

  void stopIOContext();

  void addToDestructionPromiseChain(
    SHARED_LIFETIME(VoidPromise) promise);

  void removeFromDestructionPromiseChain(
    SHARED_LIFETIME(VoidPromise) promise);

 private:
  void onAccepted(
    const ws::Listener* listener
    , ws::Listener::ErrorCode& ec
    , ws::Listener::SocketType& socket
    , std::shared_ptr<ws::Listener::StrandType> perConnectionStrand);

  void onDetected(
    std::shared_ptr<http::DetectChannel> detectChannel
    , http::DetectChannel::ErrorCode& ec
    , bool handshake_result
    , http::DetectChannel::StreamType&& stream
    , http::DetectChannel::MessageBufferType&& buffer
    , std::shared_ptr<http::DetectChannel::StrandType> perConnectionStrand);

 private:
  // The io_context is required for all I/O
  boost::asio::io_context ioc_{};

  const boost::asio::ip::address address_
    = ::boost::asio::ip::make_address("127.0.0.1");

  const unsigned short port_
    = 8085;

  const boost::asio::ip::tcp::endpoint tcpEndpoint_
      = boost::asio::ip::tcp::endpoint{
          address_, port_};

  ::boost::asio::ssl::context ctx_
    {::boost::asio::ssl::context::tlsv12};

  std::shared_ptr<ws::Listener> listener_;

  // When subscription gets deleted it will deregister callback
  std::unique_ptr<ws::Listener::AcceptedCallbackList::Subscription>
    acceptedCallbackSubscription_;

  base::RunLoop run_loop_{};

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  boost::asio::signal_set signals_set_{
    ioc_ /// \note will not handle signals if ioc stopped
    , SIGINT
    , SIGTERM
  };

  VoidPromiseContainer destructionPromises_;

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    = base::MessageLoop::current()->task_runner();

  base::Thread asio_thread_1{"asio_thread_1"};

  base::Thread asio_thread_2{"asio_thread_2"};

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ExampleServer);
};

ExampleServer::ExampleServer()
{
  DETACH_FROM_SEQUENCE(sequence_checker_);

  listener_
    = std::make_shared<ws::Listener>(
        ioc_
        , tcpEndpoint_
      );

  acceptedCallbackSubscription_
    = listener_->registerCallback(
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
        DCHECK(mainLoopRunner_);

        LOG(INFO)
          << "got stop signal";

        base::PostPromise(FROM_HERE
          , mainLoopRunner_.get()
          , base::BindOnce(
              /// \note returns promise,
              /// so we will wait for NESTED promise
              &ExampleServer::stopAcceptors,
              base::Unretained(this))
        )
        // do not accept new connections
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
            [
            ](
              const ::util::Status& stopAcceptorResult
            ){
               if(!stopAcceptorResult.ok()) {
                 LOG(ERROR)
                   << "failed to stop acceptor with status: "
                   << stopAcceptorResult.ToString();
               }
            })
        )
        // wait for destruction of existing connections
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              /// \note returns promise,
              /// so we will wait for NESTED promise
              &ExampleServer::promiseDestructionOfConnections
              , base::Unretained(this)
          )
        )
        // stop io context
        .ThenOn(mainLoopRunner_
          , FROM_HERE
          , base::BindOnce(
              /// \note returns promise,
              /// so we will wait for NESTED promise
              &ExampleServer::stopIOContext
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
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return base::Promises::All(FROM_HERE
    /// \todo all more acceptors
    , listener_->stopAcceptorAsync());
}

void ExampleServer::onAccepted(
   const ws::Listener* listener
  , ws::Listener::ErrorCode& ec
  , ws::Listener::SocketType& socket
  , std::shared_ptr<ws::Listener::StrandType> perConnectionStrand)
{
  DCHECK(listener);

  DCHECK(perConnectionStrand
    && perConnectionStrand->running_in_this_thread());

  // Handle the error, if any
  if (ec)
  {
    LOG(ERROR)
      << "Listener failed to accept new connection with error: "
      << ec.message();
    return;
  }

  LOG(INFO)
    << "Listener accepted new connection";

  std::shared_ptr<http::DetectChannel> detectChannel
    = std::make_shared<http::DetectChannel>(
        ctx_
        , std::move(socket)
        , base::BindRepeating(
            &ExampleServer::onDetected
            , base::Unretained(this)
        )
        , std::move(perConnectionStrand));

  base::OnceClosure runDetectorCb
    = base::BindOnce(
        &http::DetectChannel::runDetector
        , SHARED_LIFETIME(detectChannel)
        , std::chrono::seconds(30) // expire timeout
      );

  base::PostPromise(FROM_HERE
    , mainLoopRunner_.get()
    , base::BindOnce(
        // prevent server termination before all connections closed
        &ExampleServer::addToDestructionPromiseChain
        , base::Unretained(this)
        , SHARED_LIFETIME(detectChannel->destructionPromise()))
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
      /// \note returns promise,
      /// so we will wait for NESTED promise
      &base::PostPromiseOnAsioExecutor<
        base::OnceClosure
      >
      , FROM_HERE
      , CONST_REFERENCED(detectChannel->perConnectionStrand())
      /// \note callback must prolong lifetime of |perConnectionStrand|
      , std::move(runDetectorCb)
    ) // BindOnce
  )
  /// \note tasks below will be scheduled after channel destruction
  /// i.e. will wait for |destructionPromise|
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        /// \note returns promise,
        /// so we will wait for NESTED promise
        &http::DetectChannel::destructionPromise
        , SHARED_LIFETIME(detectChannel))
  )
  .ThenOn(mainLoopRunner_
    , FROM_HERE
    , base::BindOnce(
        // destroyed connections do not prevent server termination
        &ExampleServer::removeFromDestructionPromiseChain
        , base::Unretained(this)
        , SHARED_LIFETIME(detectChannel->destructionPromise())
      )
  );
}

void ExampleServer::onDetected(
  std::shared_ptr<http::DetectChannel> detectChannel
  , http::DetectChannel::ErrorCode& ec
  , bool handshake_result
  , http::DetectChannel::StreamType&& stream
  , http::DetectChannel::MessageBufferType&& buffer
  , std::shared_ptr<http::DetectChannel::StrandType> perConnectionStrand)
{
  DCHECK(perConnectionStrand
    && perConnectionStrand->running_in_this_thread());

  // Handle the error, if any
  if (ec)
  {
    LOG(ERROR)
      << "Handshake failed for new connection with error: "
      << ec.message();
    return;
  }

  if(handshake_result) {
    LOG(INFO)
      << "Completed secure handshake of new connection";
  } else {
    LOG(INFO)
      << "Completed NOT secure handshake of new connection";
  }

  /// \todo: create channel here

  // reset |DetectChannel| (we do not need it anymore)
  mainLoopRunner_->PostTask(FROM_HERE,
    base::BindOnce(
      [
      ](
        std::shared_ptr<http::DetectChannel> detectChannel
      ){
          detectChannel.reset();
      }
      , SHARED_LIFETIME(detectChannel)
    )
  );
}

void ExampleServer::runLoop()
{
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

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<base::SequencedTaskRunner> asio_task_runner_1 =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  /// \note will stop periodic timer on scope exit
  basis::PeriodicTaskExecutor periodicAsioExecutor_1(
    asio_task_runner_1
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
          /// \note Runs only on one sequence!
          /// In production create multiple threads
          /// to run |boost::asio::io_context|
          ioc.run_one_for(
            std::chrono::milliseconds{15});
        }
        , REFERENCED(ioc_)
    )
  );

  periodicAsioExecutor_1.startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(30));

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<base::SequencedTaskRunner> asio_task_runner_2 =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  /// \note will stop periodic timer on scope exit
  basis::PeriodicTaskExecutor periodicAsioExecutor_2(
    asio_task_runner_2
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
          /// \note Runs only on one sequence!
          /// In production create multiple threads
          /// to run |boost::asio::io_context|
          ioc.run_one_for(
            std::chrono::milliseconds{10});
        }
        , REFERENCED(ioc_)
    )
  );

  periodicAsioExecutor_2.startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(25));

  DCHECK(base::ThreadPool::GetInstance());
  scoped_refptr<base::SequencedTaskRunner> asio_task_runner_3 =
    base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      base::TaskTraits{
        base::TaskPriority::BEST_EFFORT
        , base::MayBlock()
        , base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  /// \note will stop periodic timer on scope exit
  basis::PeriodicTaskExecutor periodicAsioExecutor_3(
    asio_task_runner_3
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
          /// \note Runs only on one sequence!
          /// In production create multiple threads
          /// to run |boost::asio::io_context|
          ioc.run_one_for(
            std::chrono::milliseconds{5});
        }
        , REFERENCED(ioc_)
    )
  );

  periodicAsioExecutor_3.startPeriodicTimer(
    base::TimeDelta::FromMilliseconds(35));

  run_loop_.Run();

  asio_thread_1.Stop();
  DCHECK(!asio_thread_1.IsRunning());

  asio_thread_2.Stop();
  DCHECK(!asio_thread_2.IsRunning());
}

ExampleServer::VoidPromise ExampleServer::configureAndRunAcceptor()
{
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
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if(!destructionPromises_.empty()) {
    return base::Promises::All(FROM_HERE, destructionPromises_);
  }

  // dummy promise
  return base::Promise<void, base::NoReject>::CreateResolved(FROM_HERE);
}

void ExampleServer::stopIOContext()
{
  // Stop the io_context. This will cause run()
  // to return immediately, eventually destroying the
  // io_context and any remaining handlers in it.
  THREAD_SAFE(ioc_.stop());
  DCHECK(THREAD_SAFE(ioc_.stopped()));
}

bool ExampleServer::VoidPromiseComparator::operator()
  (const ExampleServer::VoidPromise& a
   , const ExampleServer::VoidPromise& b) const
{
  return a.GetScopedRefptrForTesting()
    < b.GetScopedRefptrForTesting();
}

void ExampleServer::addToDestructionPromiseChain(
  SHARED_LIFETIME(VoidPromise) promise)
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  destructionPromises_.emplace(promise);
}

void ExampleServer::removeFromDestructionPromiseChain(
  SHARED_LIFETIME(VoidPromise) boundPromise)
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::EraseIf(destructionPromises_,
    [
      SHARED_LIFETIME(boundPromise)
    ](
      const VoidPromise& key
    ){
      return key.GetScopedRefptrForTesting()
        == boundPromise.GetScopedRefptrForTesting();
    });
}

ExampleServer::~ExampleServer()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(THREAD_SAFE(ioc_.stopped()));

  DCHECK(
    /// \note not generally thread-safe,
    /// but assumed to be thread-safe here
    THREAD_SAFE(destructionPromises_).empty());
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
            /// \note returns promise,
            /// so we will wait for NESTED promise
            &ExampleServer::configureAndRunAcceptor
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

  /*{
    // divide by zero
    int n = 42;
    int d = 0;
    auto f = n/d;
    LOG(INFO) << f; // do not optimize out
  }*/
}
