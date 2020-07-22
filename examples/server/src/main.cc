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
#include <base/task/thread_pool/thread_pool.h>

#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>

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
          , 7 // threadsNum
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

  // The io_context is required for all I/O
  boost::asio::io_context ioc{};

  const boost::asio::ip::address address
    = ::boost::asio::ip::make_address("127.0.0.1");

  const unsigned short port
    = 8085;

  const boost::asio::ip::tcp::endpoint tcpEndpoint
      = boost::asio::ip::tcp::endpoint{
          address, port};

  ::boost::asio::ssl::context ctx
    {::boost::asio::ssl::context::tlsv12};

  ws::Listener::AcceptedCallback acceptedCallback
    = base::BindRepeating(
      [
      ](
        /// \section bound args
        ::boost::asio::ssl::context& boundCtx
        /// \section callback args
        , const ws::Listener* listener
        , ws::Listener::ErrorCode& ec
        , ws::Listener::SocketType& socket
      ){
        DCHECK(listener
          && listener->isAcceptingInThisSequence());

        http::DetectChannel::DetectedCallback detectedCallback
          = base::BindRepeating(
            [
            ](
              const http::DetectChannel* detectChannel
              , http::DetectChannel::ErrorCode& ec
              , bool handshake_result
              , http::DetectChannel::StreamType&& stream
              , http::DetectChannel::MessageBufferType&& buffer
            ){
              DCHECK(detectChannel
                && detectChannel->isDetectingInThisSequence());

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
            }
          );

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
              boundCtx
              , std::move(socket)
              , std::move(detectedCallback));

        detectChannel->runDetector();
      }
      , REFERENCED(ctx)
    );

  std::shared_ptr<ws::Listener> listener
    = std::make_shared<ws::Listener>(
        ioc
        , tcpEndpoint
      );

  // When subscription gets deleted it will deregister callback
  std::unique_ptr<ws::Listener::AcceptedCallbackList::Subscription>
    acceptedCallbackSubscription
      = listener->registerCallback(
          std::move(acceptedCallback)
        );

  base::RunLoop run_loop{};

  // Capture SIGINT and SIGTERM to perform a clean shutdown
  boost::asio::signal_set signals_set(
    ioc /// \note will not handle signals if ioc stopped
    , SIGINT
    , SIGTERM
  );

#if defined(SIGQUIT)
  signals_set.add(SIGQUIT);
#else
  #error "SIGQUIT not defined"
#endif // defined(SIGQUIT)

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner
    = base::MessageLoop::current()->task_runner();

  auto sigQuitCallback
    = [&run_loop, &listener, &mainLoopRunner]
      (boost::system::error_code const&, int)
      {
        DCHECK(mainLoopRunner);

        LOG(INFO)
          << "got stop signal";

        listener->stopAcceptorAsync()
        .ThenOn(mainLoopRunner
          , FROM_HERE
          , base::BindOnce(
            [
            ](
              const ::util::Status& stopAcceptorResult
            ){
               if(!stopAcceptorResult.ok()) {
                 LOG(WARNING)
                   << "failed to stop acceptor with status: "
                   << stopAcceptorResult.ToString();
               }
            })
        )
        .ThenOn(mainLoopRunner
          , FROM_HERE
          , run_loop.QuitClosure());
      };

  signals_set.async_wait(std::move(sigQuitCallback));

  {
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
            DCHECK(!ioc.stopped());
            /// \note Runs only on one sequence!
            /// In production create multiple threads
            /// to run |boost::asio::io_context|
            ioc.run_one_for(
              std::chrono::milliseconds{15});
          }
          , REFERENCED(ioc)
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
            DCHECK(!ioc.stopped());
            /// \note Runs only on one sequence!
            /// In production create multiple threads
            /// to run |boost::asio::io_context|
            ioc.run_one_for(
              std::chrono::milliseconds{10});
          }
          , REFERENCED(ioc)
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
            DCHECK(!ioc.stopped());
            /// \note Runs only on one sequence!
            /// In production create multiple threads
            /// to run |boost::asio::io_context|
            ioc.run_one_for(
              std::chrono::milliseconds{5});
          }
          , REFERENCED(ioc)
      )
    );

    periodicAsioExecutor_3.startPeriodicTimer(
      base::TimeDelta::FromMilliseconds(35));

    listener->configureAndRun()
    .ThenOn(mainLoopRunner
      , FROM_HERE
      , base::BindOnce(
        [
        ](
        ){
          LOG(INFO)
            << "websocket listener is running";
        }
    ));

    run_loop.Run();
  }

  /*{
    // divide by zero
    int n = 42;
    int d = 0;
    auto f = n/d;
    LOG(INFO) << f; // do not optimize out
  }*/
}
