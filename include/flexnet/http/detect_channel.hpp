#pragma once

#include "flexnet/util/limited_tcp_stream.hpp"

#include <base/callback.h>
#include <base/macros.h>
#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/memory/weak_ptr.h>
#include <base/sequence_checker.h>
#include <base/synchronization/atomic_flag.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/lock_with_check.hpp>
#include <basis/checked_optional.hpp>
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
  /// \todo make configurable
  static constexpr size_t kMaxMessageSizeByte = 100000;

public:
  using MessageBufferType
    = ::boost::beast::flat_static_buffer<kMaxMessageSizeByte>;

  using ErrorCode
    = ::boost::beast::error_code;

  using StreamType
    /// \note stream with custom rate limiter
    = ::boost::beast::limited_tcp_stream;

  using ExecutorType
    = StreamType::executor_type;

  using StrandType
    = ::boost::asio::strand<ExecutorType>;

  using IoContext
    = ::boost::asio::io_context;

  using AsioTcp
    = ::boost::asio::ip::tcp;

  // result of |beast::async_detect_ssl|
  struct SSLDetectResult {
    SSLDetectResult(
      ErrorCode&& ec
      , const bool handshakeResult
      , StreamType&& stream
      , MessageBufferType&& buffer
      , const bool _need_close)
      : ec(base::rvalue_cast(ec))
      , handshakeResult(handshakeResult)
      , stream(base::rvalue_cast(stream))
      , buffer(base::rvalue_cast(buffer))
      , need_close(_need_close)
      {}

    SSLDetectResult(
      SSLDetectResult&& other)
      : SSLDetectResult(
          base::rvalue_cast(other.ec)
          , base::rvalue_cast(other.handshakeResult)
          , base::rvalue_cast(other.stream.value())
          , base::rvalue_cast(other.buffer)
          , base::rvalue_cast(other.need_close))
      {}

    // Copy assignment operator
    SSLDetectResult& operator=(const SSLDetectResult& rhs)
      = delete;

    ~SSLDetectResult()
    {
      LOG_CALL(DVLOG(99));
    }

    // Move assignment operator
    //
    // MOTIVATION
    //
    // To use type as ECS component
    // it must be `move-constructible` and `move-assignable`
    SSLDetectResult& operator=(SSLDetectResult&& rhs)
    {
      if (this != &rhs)
      {
        ec = base::rvalue_cast(rhs.ec);
        handshakeResult = base::rvalue_cast(rhs.handshakeResult);
        DCHECK(rhs.stream.has_value());
        stream.emplace(base::rvalue_cast(rhs.stream.value()));
        buffer = base::rvalue_cast(rhs.buffer);
        need_close = base::rvalue_cast(rhs.need_close);
      }

      return *this;
    }

    ErrorCode ec;
    bool handshakeResult;
    /// \todo make NOT optional
    // can not copy assign `stream`, so use optional
    std::optional<StreamType> stream;
    MessageBufferType buffer;
    bool need_close;
  };

public:
  DetectChannel(
    // Take ownership of the socket
    AsioTcp::socket&& socket
    , ECS::AsioRegistry& asioRegistry
    , const ECS::Entity entity_id);

  DetectChannel(
    DetectChannel&& other) = delete;

  /// \note can destruct on any thread
  ~DetectChannel()
    RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(FUNC_GUARD(DetectChannelDestructor));

  // calls |beast::async_detect_*|
  void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
    = std::chrono::seconds(30));

  SET_WEAK_SELF(DetectChannel)

  MUST_USE_RETURN_VALUE
  bool isDetectingInThisThread() const NO_EXCEPTION
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(is_stream_valid_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

    /// \note |perConnectionStrand_|
    /// is valid as long as |stream_| valid
    /// i.e. valid util |stream_| moved out
    /// (it uses executor from stream).
    DCHECK(is_stream_valid_.load());

    /// \note |running_in_this_thread| is thread-safe
    /// only if |perConnectionStrand_| will not be modified concurrently
    return perConnectionStrand_->running_in_this_thread();
  }

  MUST_USE_RETURN_VALUE
  const StrandType& perConnectionStrand() NO_EXCEPTION
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(is_stream_valid_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

    /// \note |perConnectionStrand_|
    /// is valid as long as |stream_| valid
    /// i.e. valid util |stream_| moved out
    /// (it uses executor from stream).
    DCHECK(is_stream_valid_.load());

    return *perConnectionStrand_;
  }

  MUST_USE_RETURN_VALUE
  /// \note make sure that |stream_| exists
  /// and thread-safe when you call |executor()|
  boost::asio::executor executor() NO_EXCEPTION
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(stream_));

    DCHECK(stream_.has_value());
    return /// \note `get_executor` returns copy
           stream_.value().get_executor();
  }

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnStrand(
    const base::Location& from_here
    , CallbackT&& task
    , base::IsNestedPromise isNestedPromise = base::IsNestedPromise())
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(stream_));
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(perConnectionStrand_));

    DCHECK(stream_.has_value()
      && stream_.value().socket().is_open());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task)
      , isNestedPromise);
  }

  /// \note returns COPY because of thread safety reasons:
  /// `entity_id_` assumed to be NOT changed,
  /// so its copy can be read from any thread.
  /// `ECS::Entity` is just number, so can be copied freely.
  ECS::Entity entityId() const
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(entity_id_));
    return entity_id_;
  }

  bool isDetected()
  {
    DCHECK_THREAD_GUARD_SCOPE(MEMBER_GUARD(atomicDetectDoneFlag_));
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
    , bool handshakeResult
    , StreamType&& stream
    , MessageBufferType&& buffer
    , bool need_close)
    RUN_ON(&asioRegistry_->strand);

  void configureDetector
    (const std::chrono::seconds& expire_timeout);

private:
  SET_WEAK_POINTERS(DetectChannel);

  /// \todo make NOT optional
  // can not copy assign `stream`, so use optional
  base::Optional<StreamType> stream_
    /// \note moved between threads,
    /// take care of thread-safety!
    SET_CUSTOM_THREAD_GUARD(MEMBER_GUARD(stream_));

  // |stream_| moved in |onDetected|
  std::atomic<bool> is_stream_valid_
    // assumed to be thread-safe
    SET_CUSTOM_THREAD_GUARD(MEMBER_GUARD(is_stream_valid_));

  // The dynamic buffer to use during `beast::async_detect_ssl`
  MessageBufferType buffer_
    /// \note moved between threads,
    /// take care of thread-safety!
    SET_CUSTOM_THREAD_GUARD(MEMBER_GUARD(buffer_));

  // |buffer_| moved in |onDetected|
  std::atomic<bool> is_buffer_valid_
    // assumed to be thread-safe
    SET_CUSTOM_THREAD_GUARD(MEMBER_GUARD(is_buffer_valid_));

  // |stream_| and calls to |async_detect*| are guarded by strand
  basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
    SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
      MEMBER_GUARD(perConnectionStrand_)
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that stream valid
      // otherwise `::boost::asio::post` may fail.
      , base::BindRepeating(
        [
        ](DetectChannel* self) -> bool {
          DCHECK_THREAD_GUARD_SCOPE(self->MEMBER_GUARD(is_stream_valid_));
          /// \note |perConnectionStrand_|
          /// is valid as long as |stream_| valid
          /// i.e. valid util |stream_| moved out
          /// (it uses executor from stream).
          return self->is_stream_valid_.load();
        }
        , base::Unretained(this)
      ));

  // will be set by |onDetected|
  std::atomic<bool> atomicDetectDoneFlag_
    // assumed to be thread-safe
    SET_CUSTOM_THREAD_GUARD(MEMBER_GUARD(atomicDetectDoneFlag_));

  // used by |entity_id_|
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(asioRegistry_));

  // `per-connection entity`
  // i.e. per-connection data storage
  const ECS::Entity entity_id_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(entity_id_));

  /// \note can destruct on any thread
  CREATE_CUSTOM_THREAD_GUARD(FUNC_GUARD(DetectChannelDestructor));

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(DetectChannel);
};

} // namespace http
} // namespace flexnet
