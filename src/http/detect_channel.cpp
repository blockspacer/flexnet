#include "flexnet/http/detect_channel.hpp" // IWYU pragma: associated

#include "flexnet/ECS/tags.hpp"

#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/guid.h>

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

namespace beast = ::boost::beast;

namespace flexnet {
namespace http {

DetectChannel::DetectChannel(
  AsioTcp::socket&& socket
  , ECS::AsioRegistry& asioRegistry
  , const ECS::Entity entity_id)
  : stream_(base::rvalue_cast(COPY_ON_MOVE(socket)))
  , is_stream_valid_(true)
  , is_buffer_valid_(true)
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , perConnectionStrand_(
      /// \note `get_executor` returns copy
      stream_.value().get_executor())
  , atomicDetectDoneFlag_(false)
  , asioRegistry_(REFERENCED(asioRegistry))
  , entity_id_(entity_id)
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

DetectChannel::~DetectChannel()
{
  DCHECK_RUN_ON_ANY_THREAD(DetectChannelDestructor);

  LOG_CALL(DVLOG(99));
}

void DetectChannel::configureDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(stream_);

  DCHECK(isDetectingInThisThread());

  DCHECK(stream_.has_value());

  stream_.value().expires_after(expire_timeout);

  // The policy object, which is default constructed, or
  // decay-copied upon construction, is attached to the stream
  // and may be accessed through the function `rate_policy`.
  //
  // Here we set individual rate limits for reading and writing
  // limit in bytes per second
  /// \todo make configurable
  stream_.value().rate_policy().read_limit(10000);

  // limit in bytes per second
  /// \todo make configurable
  stream_.value().rate_policy().write_limit(850000);
}

// Launch the detector
void DetectChannel::runDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);
  DCHECK_CUSTOM_THREAD_GUARD(stream_);
  DCHECK_CUSTOM_THREAD_GUARD(buffer_);
  DCHECK_CUSTOM_THREAD_GUARD(is_stream_valid_);
  DCHECK_CUSTOM_THREAD_GUARD(is_buffer_valid_);

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
        /// \todo make configurable
        base::TimeDelta::FromSeconds(7)}
    , basis::PeriodicCheckUntil::CheckPeriod{
        /// \todo make configurable
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
  LOG_CALL(DVLOG(99));

  DCHECK(isDetectingInThisThread());

  DCHECK_CUSTOM_THREAD_GUARD(is_stream_valid_);
  DCHECK_CUSTOM_THREAD_GUARD(is_buffer_valid_);
  DCHECK_CUSTOM_THREAD_GUARD(stream_);
  DCHECK_CUSTOM_THREAD_GUARD(buffer_);
  DCHECK_CUSTOM_THREAD_GUARD(atomicDetectDoneFlag_);
  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

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
  DCHECK(!atomicDetectDoneFlag_.load());

#if DCHECK_IS_ON()
  DCHECK(timeoutResolver);
  timeoutResolver.Run();
#endif // DCHECK_IS_ON()

  atomicDetectDoneFlag_.store(true);

  // mark SSL detection completed
  ::boost::asio::post(
    asioRegistry_->strand()
    /// \todo use base::BindFrontWrapper
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

  // we moved `stream_` out
  is_stream_valid_.store(false);
  // we moved `buffer_` out
  is_buffer_valid_.store(false);
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
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(entity_id_);

  DCHECK(asioRegistry_->running_in_this_thread());

  DVLOG(99)
    << " detected connection as "
    << (handshakeResult ? "secure" : "unsecure");

  {
    using UniqueSSLDetectComponent
      = base::Optional<DetectChannel::SSLDetectResult>;

    // If the value already exists allow it to be re-used
    (*asioRegistry_)->remove_if_exists<
        ECS::UnusedSSLDetectResultTag
      >(entity_id_);

    UniqueSSLDetectComponent& detectResult
      = (*asioRegistry_).reset_or_create_var<UniqueSSLDetectComponent>(
            "UniqueSSLDetectComponent_" + base::GenerateGUID() // debug name
            , entity_id_
            , base::rvalue_cast(ec)
            , base::rvalue_cast(handshakeResult)
            , base::rvalue_cast(stream)
            , base::rvalue_cast(buffer));
  }
}

} // namespace http
} // namespace flexnet
