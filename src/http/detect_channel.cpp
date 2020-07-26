#include "flexnet/http/detect_channel.hpp" // IWYU pragma: associated

#include "flexnet/util/macros.hpp"

#include <base/location.h>
#include <base/logging.h>

#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context_strand.hpp>

#include <algorithm>
#include <chrono>
#include <ratio>
#include <type_traits>
#include <functional>

namespace boost::asio::ssl { class context; }

namespace beast = boost::beast;

namespace flexnet {
namespace http {

DetectChannel::DetectChannel(
  ::boost::asio::ssl::context& ctx
  , AsioTcp::socket&& socket)
  : ctx_(ctx)
  // NOTE: Following the std::move,
  // the moved-from object is in the same state
  // as if constructed using the
  // basic_stream_socket(io_service&) constructor.
  // see boost.org/doc/libs/1_54_0/doc/html/boost_asio/reference/basic_stream_socket/basic_stream_socket/overload5.html
  // i.e. it does not actually destroy |stream| by |move|
  , stream_(std::move(socket))
  , ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , destruction_promise_(FROM_HERE)
  , perConnectionStrand_(stream_.get_executor())
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

DetectChannel::CAUTION_NOT_THREAD_SAFE(~DetectChannel())
{
  LOG_CALL(VLOG(9));

  destruction_promise_.Resolve();
}

void DetectChannel::registerDetectedCallback(
  DetectChannel::DetectedCallback&& detectedCallback)
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(detectedCallback);
  detectedCallback_ = std::move(detectedCallback);
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
    boost::asio::bind_executor(perConnectionStrand_,
      std::bind(
        &DetectChannel::onDetected
        , /// \note Lifetime must be managed externally.
          /// API user can free |DetectChannel| only if
          /// that callback finished (or failed to schedule).
          UNOWNED_LIFETIME(
            COPIED(this))
        , std::placeholders::_1
        , std::placeholders::_2
      )
    )
  );
}

void DetectChannel::onDetected(
  ErrorCode ec
  , bool handshakeResult)
{
  LOG_CALL(VLOG(9));

  DCHECK(isDetectingInThisThread());

  DCHECK(stream_.socket().is_open());

  DCHECK(detectedCallback_);
  detectedCallback_.Run(
    util::ConstCopyWrapper<DetectChannel*>(this)
    , REFERENCED(ec)
    , util::ConstCopyWrapper<bool>(handshakeResult)
    , std::move(stream_)
    , std::move(buffer_)
  );
}

} // namespace http
} // namespace flexnet
