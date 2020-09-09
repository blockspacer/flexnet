#pragma once

#if 1

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
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>

#include <boost/asio/buffer.hpp>
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

/// \note Does not have `reconnect` method
/// i.e. you must create new instance when you want to reconnect.
///
// Code based on
// https://www.boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
//
// A class which represents a single http connection
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
//    i.e. result stored in ECS entity.
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
class HttpChannel
{
public:
  /// \todo make configurable
  static constexpr size_t kMaxMessageSizeByte = 100000;

  /// \todo make configurable
  static constexpr size_t kExpireTimeoutSec = 60;

public:
  using RequestBodyType
    = ::boost::beast::http::string_body;

  using ResponseToRequestType
    = ::boost::beast::http::response<RequestBodyType>;

  using ResponseEmptyType
    = ::boost::beast::http::response<::boost::beast::http::empty_body>;

  using ResponseFileType
    = ::boost::beast::http::response<::boost::beast::http::file_body>;

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

public:
  HttpChannel(
    // Take ownership of the stream
    StreamType&& stream
    // Take ownership of the buffer
    , MessageBufferType&& buffer
    , ECS::AsioRegistry& asioRegistry
    , const ECS::Entity entity_id);

  HttpChannel(
    HttpChannel&& other) = delete;

  /// \note can destruct on any thread
  NOT_THREAD_SAFE_FUNCTION()
  ~HttpChannel();

  /// \todo thread safety
  MUST_USE_RETURN_VALUE
  bool isOpen();

  void doRead();

  void doReadAsync();

  // calls |beast::async_detect_*|
  /*void runDetector(
    // Sets the timeout using |stream_.expires_after|.
    const std::chrono::seconds& expire_timeout
    = std::chrono::seconds(30));*/

  template <typename CallbackT>
  auto postTaskOnConnectionStrand(
    const base::Location& from_here
    , CallbackT&& task)
  {
    DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task));
  }

  MUST_USE_RETURN_VALUE
  base::WeakPtr<HttpChannel> weakSelf() const NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(weak_this_);

    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

  /// \note `stream_` can be moved to websocket session from http session,
  /// so we can't use it here anymore
  MUST_USE_RETURN_VALUE
  bool is_stream_valid() const
  {
    DCHECK_CUSTOM_THREAD_GUARD(is_stream_valid_);
    return is_stream_valid_.load();
  }

  /*MUST_USE_RETURN_VALUE
  bool isDetectingInThisThread() const NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(is_stream_valid_);
    DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

    /// \note |perConnectionStrand_|
    /// is valid as long as |stream_| valid
    /// i.e. valid util |stream_| moved out
    /// (it uses executor from stream).
    DCHECK(is_stream_valid_.load());

    /// \note |running_in_this_thread| is thread-safe
    /// only if |perConnectionStrand_| will not be modified concurrently
    return perConnectionStrand_->running_in_this_thread();
  }*/

  /*MUST_USE_RETURN_VALUE
  const StrandType& perConnectionStrand() NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(is_stream_valid_);
    DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

    /// \note |perConnectionStrand_|
    /// is valid as long as |stream_| valid
    /// i.e. valid util |stream_| moved out
    /// (it uses executor from stream).
    DCHECK(is_stream_valid_.load());

    return *perConnectionStrand_;
  }*/

  /*MUST_USE_RETURN_VALUE
  /// \note make sure that |stream_| exists
  /// and thread-safe when you call |executor()|
  boost::asio::executor executor() NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(stream_);

    DCHECK(stream_.has_value());
    return /// \note `get_executor` returns copy
           stream_.value().get_executor();
  }*/

  /*template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnStrand(
    const base::Location& from_here
    , CallbackT&& task)
  {
    DCHECK_CUSTOM_THREAD_GUARD(stream_);
    DCHECK_CUSTOM_THREAD_GUARD(perConnectionStrand_);

    DCHECK(stream_.has_value()
      && stream_.value().socket().is_open());
    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task));
  }*/

  /// \note returns COPY because of thread safety reasons:
  /// `entity_id_` assumed to be NOT changed,
  /// so its copy can be read from any thread.
  /// `ECS::Entity` is just number, so can be copied freely.
  ECS::Entity entityId() const
  {
    DCHECK_CUSTOM_THREAD_GUARD(entity_id_);
    return entity_id_;
  }

  /*bool isDetected()
  {
    DCHECK_CUSTOM_THREAD_GUARD(atomicDetectDoneFlag_);
    return atomicDetectDoneFlag_.load();
  }*/

private:
  void onRead(
    ErrorCode ec
    , std::size_t bytes_transferred);

  void onWrite(
    ErrorCode ec
    , std::size_t bytes_transferred
    , bool close);

  void onFail(
    ErrorCode ec
    , char const* what);

  // calls `stream_.socket().shutdown`
  void doEof();

  /*void onDetected(
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
    (const std::chrono::seconds& expire_timeout);*/

private:
  StreamType stream_
    /// \note moved between threads,
    /// take care of thread-safety!
    SET_CUSTOM_THREAD_GUARD(stream_);

  // |stream_| and calls to |async_*| are guarded by strand
  basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
    SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
      perConnectionStrand_
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that stream valid
      // otherwise `::boost::asio::post` may fail.
      , base::BindRepeating(
        [
        ](
          bool is_stream_valid
          , StreamType& stream
        ){
          /// \note |perConnectionStrand_|
          /// is valid as long as |stream_| valid
          /// i.e. valid util |stream_| moved out
          /// (it uses executor from stream).
          return is_stream_valid;
        }
        , is_stream_valid_.load()
        , REFERENCED(stream_.value())
      ));

  /// \note `stream_` can be moved to websocket session from http session
  std::atomic<bool> is_stream_valid_
    // assumed to be thread-safe
    SET_CUSTOM_THREAD_GUARD(is_stream_valid_);

  // The dynamic buffer to store recieved data
  MessageBufferType buffer_
    /// \note moved between threads,
    /// take care of thread-safety!
    SET_CUSTOM_THREAD_GUARD(buffer_);

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<HttpChannel> weak_ptr_factory_
    GUARDED_BY(sequence_checker_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  const base::WeakPtr<HttpChannel> weak_this_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(weak_this_);

  // used by |entity_id_|
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(asioRegistry_);

  // `per-connection entity`
  // i.e. per-connection data storage
  const ECS::Entity entity_id_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(entity_id_);

  // The parser is stored in an optional container so we can
  // construct it from scratch at the beginning
  // of each new message.
  boost::optional<
    ::boost::beast::http::request_parser<
      RequestBodyType>
  > parser_;

  /// \todo SSL support
  /// ::boost::asio::ssl::context

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(HttpChannel);
};

} // namespace http
} // namespace flexnet

#endif // 0
