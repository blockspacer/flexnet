#pragma once

#include "flexnet/util/checked_optional.hpp"
#include "flexnet/util/limited_tcp_stream.hpp"
#include "flexnet/util/lock_with_check.hpp"

#include <base/callback.h>
#include <base/macros.h>
#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>
#include <base/synchronization/atomic_flag.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/ECS/asio_registry.hpp>
#include <basis/promise/post_promise.h>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep
#include <basis/unowned_ref.hpp> // IWYU pragma: keep

#include <boost/beast/core.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep

#include <chrono>
#include <cstddef>

namespace base { struct NoReject; }

namespace util { template <class T> class UnownedPtr; }

namespace util { template <class T> class UnownedRef; }

namespace ECS {

// Used to process component only once
// i.e. mark already processed components
// that can be re-used by memory pool.
CREATE_ECS_TAG(UnusedSSLDetectResultTag)

} // namespace ECS

namespace flexnet {
namespace http {

// Detect a TLS client handshake on a stream.
//
// Reads from a stream to determine if a client
// handshake message is being received.
//
// MOTIVATION
//
// 1. Uses memory pool
//    i.e. avoids slow memory allocations
//    We use ECS to imitate memory pool (using `UnusedTag` component),
//    so we can easily change type of data
//    that must use memory pool.
//
// 2. Uses ECS instead of callbacks to report result
//    i.e. result of `async_detect_ssl` e.t.c. stored in ECS entity.
//    That allows to process incoming data later
//    in `batches` (cache friendly).
//    Allows to customize logic dynamically
//    i.e. to discard all queued data for disconnected client
//    just add ECS system.
//
// 3. Avoids usage of `shared_ptr`.
//    You can store per-connection data in ECS entity
//    representing individual connection
//    i.e. allows to avoid custom memory management via RAII,
//    (destroy allocated data on destruction of ECS entity).
//
// 4. Provides extra thread-safety checks
//    i.e. uses `running_in_this_thread`,
//    `base/sequence_checker.h`, e.t.c.
//
// 5. Able to integrate with project-specific libs
//    i.e. use `base::Promise`, `base::RepeatingCallback`, e.t.c.
//
// HOT-CODE PATH
//
// Use plain collbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
class DetectChannel
{
public:
  static const size_t kMaxMessageSizeBytes = 100000;

public:
  using FakeLockRunType = bool();

  using FakeLockPolicy = basis::FakeLockPolicyDebugOnly;

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
    SSLDetectResult(
      ErrorCode&& ec
      , const bool&& handshakeResult
      , StreamType&& stream
      , MessageBufferType&& buffer)
      : ec(base::rvalue_cast(ec))
      , handshakeResult(CAN_COPY_ON_MOVE("moving const") std::move(handshakeResult))
      , stream(base::rvalue_cast(stream))
      , buffer(base::rvalue_cast(buffer))
      {}

    SSLDetectResult(
      SSLDetectResult&& other)
      : SSLDetectResult(
          base::rvalue_cast(other.ec)
          , base::rvalue_cast(other.handshakeResult)
          , base::rvalue_cast(other.stream.value())
          , base::rvalue_cast(other.buffer))
      {}

    // Copy assignment operator
    SSLDetectResult& operator=(const SSLDetectResult& rhs)
      = delete;

    ~SSLDetectResult()
    {
      LOG_CALL(DVLOG(99));
    }

    /// \todo remove
    // Move assignment operator
    SSLDetectResult& operator=(SSLDetectResult&& rhs)
    {
      if (this != &rhs)
      {
        ec = base::rvalue_cast(rhs.ec);
        handshakeResult = base::rvalue_cast(rhs.handshakeResult);
        DCHECK(rhs.stream.has_value());
        stream.emplace(base::rvalue_cast(rhs.stream.value()));
        buffer = base::rvalue_cast(rhs.buffer);
      }

      return *this;
    }

    ErrorCode ec;
    bool handshakeResult;
    // can not copy assign `stream`, so use optional
    std::optional<StreamType> stream;
    MessageBufferType buffer;
  };

public:
  DetectChannel(
    // Take ownership of the socket
    AsioTcp::socket&& socket
    , ECS::AsioRegistry& asioRegistry
    , const ECS::Entity entity_id);

  DetectChannel(
    DetectChannel&& other)
    : DetectChannel(
        base::rvalue_cast(other.stream_.value().socket())
        , RAW_REFERENCED(other.asioRegistry_.Ref())
        , COPIED(other.entity_id_))
    {
      DCHECK(atomicDetectDoneFlag_.load()
        == other.atomicDetectDoneFlag_.load());

      DCHECK(is_stream_valid_.load()
        == other.is_stream_valid_.load());

      DCHECK(is_buffer_valid_.load()
        == other.is_buffer_valid_.load());

      /// \note do not move |sequence_checker_|
      DETACH_FROM_SEQUENCE(sequence_checker_);
    }

  /// \note can destruct on any thread
  NOT_THREAD_SAFE_FUNCTION()
  ~DetectChannel();

  // calls |beast::async_detect_*|
  void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
    = std::chrono::seconds(30));

  MUST_USE_RETURN_VALUE
  base::WeakPtr<DetectChannel> weakSelf() const NO_EXCEPTION
  {
    basis::AutoFakeLockWithCheck<basis::FakeLockPolicyDebugOnly, FakeLockRunType>
      auto_lock(fakeLockToUnownedPointer_);

    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

  MUST_USE_RETURN_VALUE
  bool isDetectingInThisThread() const NO_EXCEPTION
  {
    /// \note |running_in_this_thread| is thread-safe
    /// only if |perConnectionStrand_| will not be modified concurrently
    return perConnectionStrand_->running_in_this_thread();
  }

  MUST_USE_RETURN_VALUE
  const StrandType& perConnectionStrand() NO_EXCEPTION
  {
    return NOT_THREAD_SAFE_LIFETIME()
           *perConnectionStrand_;
  }

  MUST_USE_RETURN_VALUE
  /// \note make sure that |stream_| exists
  /// and thread-safe when you call |executor()|
  boost::asio::executor executor() NO_EXCEPTION
  {
    DCHECK(stream_.has_value());
    return NOT_THREAD_SAFE_LIFETIME()
           /// \note `get_executor` returns copy
           stream_.value().get_executor();
  }

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnStrand(
    const base::Location& from_here
    , CallbackT&& task)
  {
    DCHECK(stream_.has_value()
      && stream_.value().socket().is_open());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task));
  }

  /// \note returns COPY because of thread safety reasons:
  /// `entity_id_` assumed to be NOT changed,
  /// so its copy can be read from any thread.
  /// `ECS::Entity` is just number, so can be copied freely.
  ECS::Entity entity_id() const
  {
    return entity_id_;
  }

  bool isDetected()
  {
    return atomicDetectDoneFlag_.load();
  }

private:
  void onDetected(
#if DCHECK_IS_ON()
    // Used to stop periodic timer that limits execution time.
    // See `timeoutPromiseResolver.GetRepeatingResolveCallback()`.
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
    (const std::chrono::seconds& expire_timeout);

private:
  /// \note stream with custom rate limiter
  // can not copy assign `stream`, so use optional
  NOT_THREAD_SAFE_LIFETIME() // moved between threads
  base::Optional<StreamType> stream_;

  /// \todo replace with Invalidatable<>
  // |stream_| moved in |onDetected|
  std::atomic<bool> is_stream_valid_{true};

  NOT_THREAD_SAFE_LIFETIME() // moved between threads
  MessageBufferType buffer_;

  /// \todo replace with Invalidatable<>
  // |buffer_| moved in |onDetected|
  std::atomic<bool> is_buffer_valid_{true};

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<DetectChannel> weak_ptr_factory_
    GUARDED_BY(fakeLockToSequence_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  const base::WeakPtr<DetectChannel> weak_this_
    // It safe to read value from any thread
    // because its storage expected to be not modified,
    // we just need to check storage validity.
    GUARDED_BY(fakeLockToUnownedPointer_);

  /// \note It is not real lock, only annotated as lock.
  /// It just calls callback on scope entry AND exit.
  basis::FakeLockWithCheck<FakeLockRunType>
    fakeLockToUnownedPointer_ {
      BIND_UNOWNED_PTR_VALIDATOR(http::DetectChannel, this)
    };

  /// \note It is not real lock, only annotated as lock.
  /// It just calls callback on scope entry AND exit.
  basis::FakeLockWithCheck<FakeLockRunType>
    fakeLockToSequence_ {
      BIND_UNRETAINED_RUN_ON_SEQUENCE_CHECK(&sequence_checker_)
    };

  // |stream_| and calls to |async_detect*| are guarded by strand
   basis::CheckedOptional<
    StrandType
    , basis::CheckedOptionalPolicy::DebugOnly
   > perConnectionStrand_;

  /// \todo replace with CheckedOptional::forceValidToRead
  // will be set by |onDetected|
  std::atomic<bool> atomicDetectDoneFlag_{false};

  /// \todo replace with CheckedOptional::forceValidToRead
  // used by |entity_id_|
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_;

  // `per-connection entity`
  // i.e. per-connection data storage
  /// `entity_id_` assumed to be NOT changed,
  /// so it can be read from any thread.
  const ECS::Entity entity_id_{ECS::NULL_ENTITY};

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
