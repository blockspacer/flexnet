#include "flexnet/http/detect_channel.hpp" // IWYU pragma: associated

#include "flexnet/util/macros.hpp"
#include "flexnet/util/move_only.hpp"
#include "flexnet/util/unowned_ptr.hpp"

#include <base/location.h>
#include <base/logging.h>

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
  AsioTcp::socket&& socket)
  : stream_(std::move(COPY_ON_MOVE(socket)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
    weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , perConnectionStrand_(stream_.get_executor())
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NOT_THREAD_SAFE_FUNCTION()
DetectChannel::~DetectChannel()
{
  LOG_CALL(VLOG(9));

  DCHECK(destructCallback_);
  std::move(destructCallback_).Run(
    util::UnownedPtr<DetectChannel>(this));

  // we assume that |DetectChannel|
  // will be destructed only after
  // |DetectChannel::onDetected| finished
  DCHECK(ALWAYS_THREAD_SAFE(
    atomicDetectDoneFlag_.IsSet()));
}

void DetectChannel::registerDetectedCallback(
  DetectChannel::DetectedCallback&& detectedCallback)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(detectedCallback);
  detectedCallback_ = std::move(detectedCallback);
}

void DetectChannel::registerDestructCallback(
  DetectChannel::DestructCallback&& destructCallback)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(destructCallback);
  destructCallback_ = std::move(destructCallback);
}

void DetectChannel::configureDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(VLOG(9));

  DCHECK(isDetectingInThisThread());

  stream_.expires_after(expire_timeout);

  // The policy object, which is default constructed, or
  // decay-copied upon construction, is attached to the stream
  // and may be accessed through the function `rate_policy`.
  //
  // Here we set individual rate limits for reading and writing
  stream_.rate_policy().read_limit(10000); // bytes per second
  stream_.rate_policy().write_limit(850000); // bytes per second
}

// Launch the detector
void DetectChannel::runDetector(
  const std::chrono::seconds& expire_timeout)
{
  LOG_CALL(VLOG(9));

  DCHECK(isDetectingInThisThread());

  configureDetector(expire_timeout);

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
  beast::async_detect_ssl(
    stream_, // The stream to read from
    buffer_, // The dynamic buffer to use
    boost::asio::bind_executor(
      perConnectionStrand_, std::move(onDetectedCb))
    );
}

void DetectChannel::onDetected(
  const ErrorCode& ec
  , const bool& handshakeResult)
{
  LOG_CALL(VLOG(9));

  DCHECK(isDetectingInThisThread());

  DCHECK(stream_.socket().is_open());

  DCHECK(detectedCallback_);

  // we assume that |onDetected|
  // will be called only once
  DCHECK(ALWAYS_THREAD_SAFE(
    !atomicDetectDoneFlag_.IsSet()));

  atomicDetectDoneFlag_.Set();

  /// \note |detectedCallback_| can
  /// destroy |DetectChannel| object
  NOT_THREAD_SAFE_LIFETIME()
  std::move(detectedCallback_).Run(
    util::UnownedPtr<DetectChannel>(this)
    , util::MoveOnly<const ErrorCode>::copyFrom(ec)
    , util::MoveOnly<const bool>::copyFrom(handshakeResult)
    , std::move(stream_)
    , std::move(buffer_)
    );
}

} // namespace http
} // namespace flexnet
