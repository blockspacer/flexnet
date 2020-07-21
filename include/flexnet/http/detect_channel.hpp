#pragma once

#include "flexnet/util/limited_tcp_stream.hpp"

#include <boost/asio.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <base/macros.h>
#include <base/callback.h>
#include <base/sequenced_task_runner.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace flexnet {
namespace http {

/** Detect a TLS client handshake on a stream.

    Reads from a stream to determine if a client
    handshake message is being received.
*/
class DetectChannel
  : public std::enable_shared_from_this<DetectChannel>
{
public:
  static const size_t kMaxMessageSizeBytes = 100000;

public:
  using MessageBufferType
    = ::boost::beast::flat_static_buffer<kMaxMessageSizeBytes>;

  using ErrorCode
    = ::boost::beast::error_code;

  using StreamType
    = ::boost::beast::limited_tcp_stream;

  using DetectedCallback
    = base::RepeatingCallback<
        void(
          std::shared_ptr<DetectChannel>
          , ::boost::beast::error_code*
          // handshake result
          // i.e. `true` if the buffer contains a TLS client handshake
          // and no error occurred, otherwise `false`.
          , bool
          , StreamType&& stream
          , MessageBufferType&& buffer)
      >;

  using AsioTcp
    = boost::asio::ip::tcp;

public:
  DetectChannel(
    ::boost::asio::ssl::context& ctx
    // Take ownership of the socket
    , AsioTcp::socket&& socket
    , DetectedCallback&& detectedCallback);

  ~DetectChannel();

  // calls |beast::async_detect_*|
  void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
      = std::chrono::seconds(30));

private:
  void onDetected(
    ErrorCode ec
    // `true` if the buffer contains a TLS client handshake
    // and no error occurred, otherwise `false`.
    , bool handshake_result);

  void configureDetector
    (const std::chrono::seconds &expire_timeout);

private:
  ::boost::asio::ssl::context& ctx_;

  /// \note stream with custom rate limiter
  StreamType stream_;

  DetectedCallback detectedCallback_;

  MessageBufferType buffer_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
