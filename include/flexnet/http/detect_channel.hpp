#pragma once

#include "flexnet/util/limited_tcp_stream.hpp"
#include "flexnet/util/macros.hpp"
#include "flexnet/util/move_only.hpp"
#include "flexnet/util/unowned_ptr.hpp" // IWYU pragma: keep

#include <base/callback.h>
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>
#include <base/synchronization/atomic_flag.h>

#include <boost/beast/core.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep

#include <basis/promise/promise.h>

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

  using DetectedCallback
    = base::OnceCallback<
    void (
      util::UnownedPtr<DetectChannel>&&
      , util::MoveOnly<const ErrorCode>&&
      // handshake result
      // i.e. `true` if the buffer contains a TLS client handshake
      // and no error occurred, otherwise `false`.
      , util::MoveOnly<const bool>&&
      , StreamType&& stream
      , MessageBufferType&& buffer)
    >;

  using DestructCallback
    = base::OnceCallback<
    void (
      util::UnownedPtr<DetectChannel>&&)
    >;

  using AsioTcp
    = ::boost::asio::ip::tcp;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

public:
  DetectChannel(
    // Take ownership of the socket
    AsioTcp::socket&& socket);

  /// \note can destruct on any thread
  NOT_THREAD_SAFE_FUNCTION()
  ~DetectChannel();

  void registerDetectedCallback(
    DetectedCallback&& detectedCallback);

  void registerDestructCallback(
    DestructCallback&& destructCallback);

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
    const ErrorCode& ec
    // `true` if the buffer contains a TLS client handshake
    // and no error occurred, otherwise `false`.
    , const bool& handshakeResult);

  void configureDetector
    (const std::chrono::seconds &expire_timeout);

private:
  /// \note stream with custom rate limiter
  NOT_THREAD_SAFE_LIFETIME()
  StreamType stream_;

  /// \note take care of thread-safety
  NOT_THREAD_SAFE_LIFETIME()
  DetectedCallback detectedCallback_;

  /// \note take care of thread-safety
  NOT_THREAD_SAFE_LIFETIME()
  DestructCallback destructCallback_;

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

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
