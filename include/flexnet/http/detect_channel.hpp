#pragma once

#include "flexnet/util/limited_tcp_stream.hpp"
#include "flexnet/ECS/asio_registry.hpp"

#include <base/callback.h>
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>
#include <base/synchronization/atomic_flag.h>

#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep

#include <boost/beast/core.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep

#include <chrono>
#include <cstddef>

namespace base { struct NoReject; }

namespace util { template <class T> class UnownedPtr; }

namespace flexnet {
namespace http {

/** Detect a TLS client handshake on a stream.

    Reads from a stream to determine if a client
    handshake message is being received.
*/
class DetectChannel
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
    = ::boost::asio::strand<StreamType::executor_type>;

  using IoContext
    = ::boost::asio::io_context;

  using AsioTcp
    = ::boost::asio::ip::tcp;

  // result of |beast::async_detect_ssl|
  struct SSLDetectResult {
    explicit SSLDetectResult(
      ErrorCode&& ec
      , const bool&& handshakeResult
      , StreamType&& stream
      , MessageBufferType&& buffer)
      : ec(std::move(ec))
      , handshakeResult(std::move(handshakeResult))
      , stream(std::move(stream))
      , buffer(std::move(buffer))
      {}

    ErrorCode ec;
    bool handshakeResult;
    StreamType stream;
    MessageBufferType buffer;
  };

public:
  DetectChannel(
    // Take ownership of the socket
    AsioTcp::socket&& socket
    , ECS::AsioRegistry& asioRegistry
    , const ECS::Entity entity_id);

  /// \note can destruct on any thread
  NOT_THREAD_SAFE_FUNCTION()
  ~DetectChannel();

  // calls |beast::async_detect_*|
  void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
    = std::chrono::seconds(30));

  MUST_USE_RETURN_VALUE
  base::WeakPtr<DetectChannel> weakSelf() const noexcept
  {
    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

  MUST_USE_RETURN_VALUE
  bool isDetectingInThisThread() const noexcept
  {
    /// \note |running_in_this_thread| is thread-safe
    /// only if |perConnectionStrand_| will not be modified concurrently
    return perConnectionStrand_.running_in_this_thread();
  }

  MUST_USE_RETURN_VALUE
  StrandType& perConnectionStrand() noexcept{
    return NOT_THREAD_SAFE_LIFETIME()
           perConnectionStrand_;
  }

  MUST_USE_RETURN_VALUE
  /// \note make sure that |stream_| exists
  /// and thread-safe when you call |executor()|
  boost::asio::executor executor() noexcept {
    return NOT_THREAD_SAFE_LIFETIME()
           stream_.get_executor();
  }

private:
  void onDetected(
#if DCHECK_IS_ON()
    COPIED() base::RepeatingClosure timeoutResolver
#endif // DCHECK_IS_ON()
    , const ErrorCode& ec
    // `true` if the buffer contains a TLS client handshake
    // and no error occurred, otherwise `false`.
    , const bool& handshakeResult);

  // uses `per-connection entity` to
  // store result of |beast::async_detect_ssl|
  void setSSLDetectResult(
    ErrorCode&& ec
    // handshake result
    // i.e. `true` if the buffer contains a TLS client handshake
    // and no error occurred, otherwise `false`.
    , bool&& handshakeResult
    , StreamType&& stream
    , MessageBufferType&& buffer);

  void configureDetector
    (const std::chrono::seconds &expire_timeout);

private:
  /// \note stream with custom rate limiter
  NOT_THREAD_SAFE_LIFETIME()
  StreamType stream_;

  NOT_THREAD_SAFE_LIFETIME()
  MessageBufferType buffer_;

  // Provides I/O functionality
  GLOBAL_THREAD_SAFE_LIFETIME()
  util::UnownedPtr<IoContext> ioc_;

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<
    DetectChannel
    > weak_ptr_factory_
  LIVES_ON(sequence_checker_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  base::WeakPtr<DetectChannel> weak_this_
  LIVES_ON(sequence_checker_);

  // |stream_| and calls to |async_detect*| are guarded by strand
  NOT_THREAD_SAFE_LIFETIME()
  StrandType perConnectionStrand_;

  // will be set by |onDetected|
  base::AtomicFlag atomicDetectDoneFlag_{};

  // used by |entity_id_|
  ECS::AsioRegistry& asioRegistry_;

  // `per-connection entity`
  // i.e. per-connection data storage
  ECS::Entity entity_id_;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
