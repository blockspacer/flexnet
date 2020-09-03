#include "flexnet/http/detect_channel.hpp" // IWYU pragma: associated

#include "flexnet/ECS/tags.hpp"

#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>

#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/promise/post_promise.h>

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/bind_executor.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <ratio>
#include <type_traits>

namespace beast = boost::beast;

namespace flexnet {
namespace http {

DetectChannel::DetectChannel(
  AsioTcp::socket&& socket
  , ECS::AsioRegistry& asioRegistry
  , const ECS::Entity entity_id)
  : stream_(base::rvalue_cast(COPY_ON_MOVE(socket)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(
        BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
        , base::in_place
        , COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
        , base::in_place
        , weak_ptr_factory_.ref_value_unsafe(
            FROM_HERE, "access from constructor").GetWeakPtr()))
  , perConnectionStrand_(
      // on each access to strand check that ioc not stopped
      // otherwise `::boost::asio::post` may fail
      base::BindRepeating(
        [](bool is_stream_valid, StreamType& stream)
        {
          /// \note |perConnectionStrand_|
          /// is valid as long as |stream_| valid
          /// i.e. valid util |stream_| moved out
          /// (it uses executor from stream).
          return is_stream_valid;
        }
        , REFERENCED(is_stream_valid_)
        /// \note `get_executor` returns copy
        , REFERENCED(stream_.value())
      )
      , base::in_place
      /// \note `get_executor` returns copy
      , stream_.value().get_executor())
  , asioRegistry_(REFERENCED(asioRegistry))
  , entity_id_(entity_id)
{
  LOG_CALL(DVLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NOT_THREAD_SAFE_FUNCTION()
DetectChannel::~DetectChannel()
{
  LOG_CALL(DVLOG(9));
}

void DetectChannel::configureDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(DVLOG(9));

  DCHECK(isDetectingInThisThread());

  DCHECK(stream_.has_value());

  stream_.value().expires_after(expire_timeout);

  // The policy object, which is default constructed, or
  // decay-copied upon construction, is attached to the stream
  // and may be accessed through the function `rate_policy`.
  //
  // Here we set individual rate limits for reading and writing
  // limit in bytes per second
  stream_.value().rate_policy().read_limit(10000);

  // limit in bytes per second
  stream_.value().rate_policy().write_limit(850000);
}

// Launch the detector
void DetectChannel::runDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(DVLOG(9));

  DCHECK(isDetectingInThisThread());

  configureDetector(expire_timeout);

  // Performs extra checks using bad for performance things
  // like `Promise` and sequence-local periodic timer,
  // so we enable extra checks only in debug mode.
#if DCHECK_IS_ON()
  // used to limit execution time of async function
  // that resolves promise
  base::ManualPromiseResolver<void, base::NoReject>
    timeoutPromiseResolver(FROM_HERE);

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
        base::TimeDelta::FromSeconds(7)}
    , basis::PeriodicCheckUntil::CheckPeriod{
        base::TimeDelta::FromMinutes(1)}
    , "detection of new connection hanged")));

  /// \note promise has shared lifetime,
  /// so we expect it to exist until (at least)
  /// it is resolved using `GetRepeatingResolveCallback`
  timeoutPromiseResolver
  .promise()
  // reset check of execution time
  .ThenOn(timeout_task_runner
    , FROM_HERE
    , base::BindOnce(&basis::unsetPeriodicTimeoutCheckerOnSequence)
  )
  ;
#endif // DCHECK_IS_ON()

  /// \note Lifetime of async callbacks
  /// must be managed externally.
  /// API user can free |DetectChannel| only if
  /// all its callbacks finished (or failed to schedule).
  /// i.e. API user must wait for |destruction_promise_|
  auto onDetectedCb
    = std::bind(
        &DetectChannel::onDetected
        , UNOWNED_LIFETIME(
          COPIED(this))
#if DCHECK_IS_ON()
        , timeoutPromiseResolver.GetRepeatingResolveCallback()
#endif // DCHECK_IS_ON()
        , std::placeholders::_1
        , std::placeholders::_2
        );

  /** Detect a TLS/SSL handshake asynchronously on a stream.

      This function reads asynchronously from a stream to determine
      if a client handshake message is being received.

      This call always returns immediately. The asynchronous operation
      will continue until one of the following conditions is true:

      @li A TLS client opening handshake is detected,

      @li The received data is invalid for a TLS client handshake, or

      @li An error occurs.

      The algorithm, known as a <em>composed asynchronous operation</em>,
      is implemented in terms of calls to the next layer's `async_read_some`
      function. The program must ensure that no other calls to
      `async_read_some` are performed until this operation completes.

      Bytes read from the stream will be stored in the passed dynamic
      buffer, which may be used to perform the TLS handshake if the
      detector returns true, or be otherwise consumed by the caller based
      on the expected protocol.
  */
  DCHECK(stream_.has_value());
  DCHECK(is_stream_valid_.load());
  DCHECK(is_buffer_valid_.load());
  beast::async_detect_ssl(
    RAW_REFERENCED(stream_.value()), // The stream to read from
    RAW_REFERENCED(buffer_), // The dynamic buffer to use
    boost::asio::bind_executor(
      *perConnectionStrand_
      , base::rvalue_cast(onDetectedCb))
    );
}

void DetectChannel::onDetected(
#if DCHECK_IS_ON()
  COPIED() base::RepeatingClosure timeoutResolver
#endif // DCHECK_IS_ON()
  , const ErrorCode& ec
  , const bool& handshakeResult)
{
  LOG_CALL(DVLOG(9));

  DCHECK(isDetectingInThisThread());

  DCHECK(is_stream_valid_.load());
  DCHECK(is_buffer_valid_.load());

#if DCHECK_IS_ON()
  if(!ec) {
    CHECK(stream_.has_value());
    if(!stream_.value().socket().is_open()) {
      LOG_CALL(DVLOG(99))
        << "detected closed socket";
    }
  }
#endif // DCHECK_IS_ON()

  // we assume that |onDetected|
  // will be called only once
  DCHECK(ALWAYS_THREAD_SAFE(
    !atomicDetectDoneFlag_.load()));

#if DCHECK_IS_ON()
  DCHECK(timeoutResolver);
  timeoutResolver.Run();
#endif // DCHECK_IS_ON()

  atomicDetectDoneFlag_ = true;

  // mark SSL detection completed
  ::boost::asio::post(
    asioRegistry_->ref_strand(FROM_HERE)
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& boundTask
      ){
        base::rvalue_cast(boundTask).Run();
      }
      , base::BindOnce(
          &DetectChannel::setSSLDetectResult
          , base::Unretained(this)
          /// \note do not forget to free allocated resources
          /// in case of error code
          , CAN_COPY_ON_MOVE("moving const") std::move(ec)
          , CAN_COPY_ON_MOVE("moving const") std::move(handshakeResult)
          , MAKES_INVALID(stream_) base::rvalue_cast(stream_.value())
          , MAKES_INVALID(buffer_) base::rvalue_cast(buffer_)
        )
    )
  );
}

void DetectChannel::setSSLDetectResult(
  ErrorCode&& ec
  // handshake result
  // i.e. `true` if the buffer contains a TLS client handshake
  // and no error occurred, otherwise `false`.
  , bool&& handshakeResult
  , StreamType&& stream
  , MessageBufferType&& buffer)
{
  DCHECK(asioRegistry_->running_in_this_thread());

  DVLOG(1)
    << " detected connection as "
    << (handshakeResult ? "secure" : "unsecure");

  ECS::Registry& registry
    = asioRegistry_->ref_registry(FROM_HERE);

  using UniqueSSLDetectComponent
    = base::Optional<DetectChannel::SSLDetectResult>;

  const bool useCache
    = registry.has<UniqueSSLDetectComponent>(entity_id_);

  registry.remove_if_exists<
    ECS::UnusedSSLDetectResultTag
  >(entity_id_);

  UniqueSSLDetectComponent& detectResult
    = useCache
      ? registry.get<UniqueSSLDetectComponent>(entity_id_)
      : registry.emplace<UniqueSSLDetectComponent>(
          entity_id_
          , base::in_place
          , base::rvalue_cast(ec)
          , base::rvalue_cast(handshakeResult)
          , base::rvalue_cast(stream)
          , base::rvalue_cast(buffer));

  if(useCache) {
    DCHECK(detectResult);
    detectResult.emplace(
          base::rvalue_cast(ec)
          , base::rvalue_cast(handshakeResult)
          , base::rvalue_cast(stream)
          , base::rvalue_cast(buffer));
    DVLOG(99)
      << "using preallocated SSLDetectResult";
  } else {
    DVLOG(99)
      << "allocating new SSLDetectResult";
  }
}

} // namespace http
} // namespace flexnet
