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

#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
#include <basis/scoped_checks.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/promise/post_promise.h>
#include <basis/status/statusor.hpp>
#include <basis/move_only.hpp>
#include <basis/atomic_flag_macros.hpp>
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
/// \todo
//CREATE_ECS_TAG(UnusedSSLDetectResultTag)

} // namespace ECS

namespace flexnet {
namespace http {

/// \note Does not have `reconnect` method
/// i.e. you must create new instance when you want to reconnect.
///
// Code based on
// boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
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
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
class HttpChannel
{
public:
  /// \todo make configurable
  static constexpr size_t kMaxMessageSizeByte = 100000;

  /// \todo make configurable
  // Timeout for inactive connection
  static constexpr size_t kExpireTimeoutSec = 20;

  /// \todo make configurable
  // Timeout during shutdown
  static constexpr size_t kCloseTimeoutSec = 5;

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
    , ECS::NetworkRegistry& netRegistry
    , const ECS::Entity entity_id);

  HttpChannel(
    HttpChannel&& other) = delete;

  ~HttpChannel();

  /// \todo thread safety
  MUST_USE_RETURN_VALUE
  bool isOpen() NO_EXCEPTION;

  void startReadAsync() NO_EXCEPTION;

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnConnectionStrand(
    const base::Location& from_here
    , CallbackT&& task
    , base::IsNestedPromise isNestedPromise = base::IsNestedPromise())
  NO_EXCEPTION
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(perConnectionStrand_);

    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task)
      , isNestedPromise);
  }

  SET_WEAK_SELF(HttpChannel)

  /// \note `stream_` can be moved to websocket session from http session,
  /// so we can't use it here anymore
  MUST_USE_RETURN_VALUE
  bool isStreamValid() const NO_EXCEPTION
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(is_stream_valid_);
    return is_stream_valid_.load();
  }

  /// \note returns COPY because of thread safety reasons:
  /// `entity_id_` assumed to be NOT changed,
  /// so its copy can be read from any thread.
  /// `ECS::Entity` is just number, so can be copied freely.
  MUST_USE_RETURN_VALUE
  ECS::Entity entityId() const NO_EXCEPTION
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(entity_id_);
    return entity_id_;
  }

private:
  template<
    // `ResponseType` may be `ResponseFileType`, `ResponseToRequestType`, etc.
    typename ResponseType
  >
  void processWrittenResponse(
    std::shared_ptr<
      ResponseType
    > shared_resp
    , ErrorCode ec
    , std::size_t bytes)
  {
    LOG_CALL(DVLOG(99));

    DCHECK_MEMBER_OF_UNKNOWN_THREAD(is_stream_valid_);

    DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

    DCHECK(is_stream_valid_.load());

    DCHECK(shared_resp);

    onWrite(ec, bytes, shared_resp->need_eof());
  }

  void handleWebsocketUpgrade(
    ErrorCode ec
    , std::size_t bytes_transferred
    , StreamType&& stream
    , boost::beast::http::request<
        RequestBodyType
      >&& req) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(*netRegistry_);

  void doRead() NO_EXCEPTION;

  // Inits resources before first call to `doRead()`
  void startRead() NO_EXCEPTION;

  void onRead(
    ErrorCode ec
    , std::size_t bytes_transferred) NO_EXCEPTION;

  void onWrite(
    ErrorCode ec
    , std::size_t bytes_transferred
    , bool close) NO_EXCEPTION;

  void onFail(
    ErrorCode ec
    , char const* what) NO_EXCEPTION;

  // calls `stream_.socket().shutdown`
  void doEof() NO_EXCEPTION;

  /// \note static because `this` may be freed
  /// if user issued `stop` (`stop` usually calls `doEof`)
  /// i.e. already marked with `ECS::UnusedTag`,
  /// but we scheduled `doEof`
  /// (attempted to mark with `ECS::UnusedTag` twice).
  static void markUnused(
    /// \note take care of lifetime
    ECS::NetworkRegistry& netRegistry
    , ECS::EntityId entity_id) NO_EXCEPTION;

private:
  SET_WEAK_POINTERS(HttpChannel);

  StreamType stream_
    GUARDED_BY(perConnectionStrand_);

  // |stream_| and calls to |async_*| are guarded by strand
  basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
    GUARD_MEMBER_WITH_CHECK(
      perConnectionStrand_
      // 1. It safe to read value from any thread
      // because its storage expected to be not modified.
      // 2. On each access to strand check that stream valid
      // otherwise `::boost::asio::post` may fail.
      , base::BindRepeating(
          /// \note |perConnectionStrand_|
          /// is valid as long as |stream_| valid
          /// i.e. valid util |stream_| moved out
          /// (it uses executor from stream).
          &HttpChannel::isStreamValid
          , base::Unretained(this)
        )
    );

  /// \note Object invalidation split between threads (see `markUnused`),
  /// so we want to prohibit callback execution
  /// while performing object invalidation.
  DEBUG_ATOMIC_FLAG(can_schedule_callbacks_)
    // assumed to be thread-safe
    GUARD_MEMBER_OF_UNKNOWN_THREAD(can_schedule_callbacks_);

  /// \note `stream_` can be moved to websocket session from http session
  std::atomic<bool> is_stream_valid_
    // assumed to be thread-safe
    GUARD_MEMBER_OF_UNKNOWN_THREAD(is_stream_valid_);

  // The dynamic buffer to store recieved data
  MessageBufferType buffer_
    GUARDED_BY(perConnectionStrand_);

  // used by |entity_id_|
  util::UnownedRef<ECS::NetworkRegistry> netRegistry_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(netRegistry_);

  // `per-connection entity`
  // i.e. per-connection data storage
  const ECS::Entity entity_id_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(entity_id_);

  // The parser is stored in an optional container so we can
  // construct it from scratch at the beginning
  // of each new message.
  boost::optional<
    ::boost::beast::http::request_parser<
      RequestBodyType>
  > parser_
    GUARDED_BY(perConnectionStrand_);

  /// \todo SSL support
  /// ::boost::asio::ssl::context

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(HttpChannel);
};

} // namespace http
} // namespace flexnet
