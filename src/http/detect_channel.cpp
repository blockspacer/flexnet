#include "flexnet/http/detect_channel.hpp" // IWYU pragma: associated

#include "flexnet/util/macros.hpp"

#include <boost/asio.hpp>

#include <boost/assert.hpp>

#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <boost/core/ignore_unused.hpp>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <base/logging.h>
#include <base/bind.h>
#include <base/logging.h>

namespace beast = boost::beast;

namespace flexnet {
namespace http {

DetectChannel::DetectChannel(
  ::boost::asio::ssl::context& ctx
  , AsioTcp::socket&& socket
  , DetectedCallback&& detectedCallback)
  : ctx_(ctx)
  // NOTE: Following the std::move,
  // the moved-from object is in the same state
  // as if constructed using the
  // basic_stream_socket(io_service&) constructor.
  // see boost.org/doc/libs/1_54_0/doc/html/boost_asio/reference/basic_stream_socket/basic_stream_socket/overload5.html
  // i.e. it does not actually destroy |stream| by |move|
  , stream_(std::move(socket))
  , detectedCallback_(std::move(detectedCallback))
{
  LOG_CALL(VLOG(9));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

DetectChannel::~DetectChannel()
{
  LOG_CALL(VLOG(9));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void DetectChannel::configureDetector(
  const std::chrono::seconds& expire_timeout)
{
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

  configureDetector(expire_timeout);

  beast::async_detect_ssl(
    stream_,
    buffer_,
    beast::bind_front_handler(
      &DetectChannel::onDetected,
      shared_from_this()));
}

void DetectChannel::onDetected(
  ErrorCode ec
  , bool handshake_result)
{
  LOG_CALL(VLOG(9));

  DCHECK(detectedCallback_);
  detectedCallback_.Run(
    shared_from_this()
    , &ec
    , handshake_result
    , std::move(stream_)
    , std::move(buffer_));
}

} // namespace http
} // namespace flexnet
