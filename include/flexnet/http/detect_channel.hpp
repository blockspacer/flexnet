#pragma once

#include "flexnet/util/limited_tcp_stream.hpp"

#include <base/callback.h>
#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/memory/weak_ptr.h>

#include <boost/beast/core.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/asio/io_service.hpp>

#include <basis/promise/promise.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace base { struct NoReject; }

namespace boost::asio::ssl { class context; }

namespace flexnet {
namespace http {

/** Detect a TLS client handshake on a stream.

    Reads from a stream to determine if a client
    handshake message is being received.
*/
class DetectChannel
  /// \todo remove shared_ptr overhead
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

  using StrandType
    = ::boost::asio::io_service::strand;

  using DetectedCallback
    = base::RepeatingCallback<
        void(
          std::shared_ptr<http::DetectChannel>
          , ErrorCode&
          // handshake result
          // i.e. `true` if the buffer contains a TLS client handshake
          // and no error occurred, otherwise `false`.
          , bool
          , StreamType&& stream
          , MessageBufferType&& buffer
          , std::shared_ptr<StrandType> perConnectionStrand)
      >;

  using AsioTcp
    = ::boost::asio::ip::tcp;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

public:
  DetectChannel(
    ::boost::asio::ssl::context& ctx
    // Take ownership of the socket
    , AsioTcp::socket&& socket
    , DetectedCallback&& detectedCallback
    , std::shared_ptr<StrandType> perConnectionStrand);

  ~DetectChannel();

  // calls |beast::async_detect_*|
  void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
      = std::chrono::seconds(30));

  base::WeakPtr<DetectChannel> weakSelf() const noexcept
  {
    return weak_this_;
  }

  StrandType& perConnectionStrand() const noexcept{
    return *perConnectionStrand_.get();
  }

  /// \note make sure that |stream_| exists
  /// and thread-safe when you call |executor()|
  boost::asio::executor executor() noexcept {
    return stream_.get_executor();
  }

  VoidPromise destructionPromise() noexcept
  {
    return destruction_promise_.promise();
  }

private:
  void onDetected(
    ErrorCode ec
    // `true` if the buffer contains a TLS client handshake
    // and no error occurred, otherwise `false`.
    , bool handshake_result);

  void configureDetector
    (const std::chrono::seconds &expire_timeout);

private:
  /// \todo use it, add SSL support
  ::boost::asio::ssl::context& ctx_;

  /// \note stream with custom rate limiter
  StreamType stream_;

  DetectedCallback detectedCallback_;

  MessageBufferType buffer_;

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<
      DetectChannel
    > weak_ptr_factory_;

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  base::WeakPtr<DetectChannel> weak_this_;

  base::ManualPromiseResolver<void, base::NoReject>
    destruction_promise_;

  // |stream_| and calls to |async_detect*| are guarded by strand
  /// \todo remove shared_ptr overhead
  std::shared_ptr<StrandType> perConnectionStrand_;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
