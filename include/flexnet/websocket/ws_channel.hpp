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
#include <base/containers/circular_deque.h>
#include <base/recursion_checker.h>

#include <basis/checked_optional.hpp>
#include <basis/atomic_flag_macros.hpp>
#include <basis/lock_with_check.hpp>
#include <basis/ECS/network_registry.hpp>
#include <basis/promise/post_promise.h>
#include <basis/status/statusor.hpp>
#include <basis/move_only.hpp>
#include <basis/task/task_util.hpp>
#include <basis/unowned_ptr.hpp> // IWYU pragma: keep
#include <basis/unowned_ref.hpp> // IWYU pragma: keep

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/circular_buffer.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
namespace ws {

/// \todo support both SslWebsocketSession and PlainWebsocketSession
/// as in github.com/iotaledger/hub/blob/master/common/http_server_base.cc#L203
///
/// \note Does not have `reconnect` method
/// i.e. you must create new instance when you want to reconnect.
///
// Code based on
// boost.org/doc/libs/1_71_0/libs/beast/example/websocket/server/async/websocket_server_async.cpp
//
// A class which represents a single websocket connection
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
class WsChannel
{
public:
  /// \todo make configurable
  static constexpr size_t kMaxMessageSizeByte = 100000;

  /// \todo make configurable
  // max. num. of scheduled messages per one connection
  static constexpr size_t kMaxSendQueueSize = 1000;

  /// \todo make configurable
  // Timeout during shutdown
  static constexpr size_t kCloseTimeoutSec = 5;

public:
  template<class Body, class Allocator>
  using UpgradeRequestType
    = boost::beast::http::request<
        Body
        , boost::beast::http::basic_fields<Allocator>
      >;

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

  /// \note we want to reduce copies for performance reasons,
  /// so force user to use `std::shared_ptr` copies
  /// or moves like so:
  /// std::make_shared<const std::string>(base::rvalue_cast(some_text));
  using SharedMessageData
    = std::shared_ptr<const std::string>;

  struct QueuedMessage {
    SharedMessageData data;
    bool is_binary;
  };

  // max. size limit that can be configured at runtime.
  class CircularMessageBuffer
  {
   public:
    typedef QueuedMessage ValueType;

   public:
    CircularMessageBuffer(uint32_t size)
      PUBLIC_METHOD_RUN_ON(&sequence_checker_)
      : storage_(size)
    {
      DETACH_FROM_SEQUENCE(sequence_checker_);
    }

   ~CircularMessageBuffer()
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);
   }

   template <class... Args>
   void writeBack(Args&&... args) NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     storage_.push_back(std::forward<Args>(args)...);
   }

   MUST_USE_RESULT
   ValueType* frontPtr() NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return &storage_.front();
   }

   MUST_USE_RESULT
   ValueType* backPtr() NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return &storage_.back();
   }

   void popFront() NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     storage_.pop_front();
   }

   void popBack() NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     storage_.pop_back();
   }

   MUST_USE_RESULT
   size_t isEmpty() const NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return storage_.empty();
   }

   MUST_USE_RESULT
   size_t isFull() const NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return storage_.full();
   }

   MUST_USE_RESULT
   size_t size() const NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return storage_.size();
   }

   // maximum number of items in the queue.
   MUST_USE_RESULT
   size_t capacity() const NO_EXCEPTION
     PUBLIC_METHOD_RUN_ON(&sequence_checker_)
   {
     DCHECK_RUN_ON(&sequence_checker_);

     return storage_.capacity();
   }

   private:
    boost::circular_buffer<ValueType> storage_
      GUARDED_BY(&sequence_checker_);

    // check sequence on which class was constructed/destructed/configured
    SEQUENCE_CHECKER(sequence_checker_);

    DISALLOW_COPY_AND_ASSIGN(CircularMessageBuffer);
  };

  // result of |acceptor_.async_accept(...)|
  // i.e. stores created socket
  struct RecievedData {
    RecievedData(
      std::string&& _data)
      : data(base::rvalue_cast(_data))
      {}

    RecievedData(
      RecievedData&& other)
      : RecievedData(
          base::rvalue_cast(other.data))
      {}

    // Move assignment operator
    //
    // MOTIVATION
    //
    // To use type as ECS component
    // it must be `move-constructible` and `move-assignable`
    RecievedData& operator=(
      RecievedData&& rhs)
    {
      if (this != &rhs)
      {
        data = base::rvalue_cast(rhs.data);
      }

      return *this;
    }

    ~RecievedData()
    {
      LOG_CALL(DVLOG(99));
    }

    std::string data;
  };

  /*// result of |acceptor_.async_accept(...)|
  // i.e. stores created socket
  struct RecievedFrom {
    RecievedFrom(
      ECS::EntityId _entity_id)
      : entity_id(_entity_id)
      {}

    RecievedFrom(
      RecievedFrom&& other)
      : RecievedFrom(
          base::rvalue_cast(other.entity_id))
      {}

    // Move assignment operator
    //
    // MOTIVATION
    //
    // To use type as ECS component
    // it must be `move-constructible` and `move-assignable`
    RecievedFrom& operator=(
      RecievedFrom&& rhs)
    {
      if (this != &rhs)
      {
        entity_id = base::rvalue_cast(rhs.entity_id);
      }

      return *this;
    }

    ~RecievedFrom()
    {
      LOG_CALL(DVLOG(99));
    }

    ECS::EntityId entity_id;
  };*/

public:
  WsChannel(
    // Take ownership of the stream
    StreamType&& stream
    , ECS::NetworkRegistry& netRegistry
    , const ECS::Entity entity_id);

  WsChannel(
    WsChannel&& other) = delete;

  ~WsChannel();

  SET_WEAK_SELF(WsChannel)

  // Start the asynchronous operation
  template<class Body, class Allocator>
  void startAcceptAsync(
    // Clients sends the HTTP request
    // asking for a WebSocket connection
    UpgradeRequestType<Body, Allocator>&& req) NO_EXCEPTION
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(perConnectionStrand_);
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(can_schedule_callbacks_);

    DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
    ::boost::asio::post(
      *perConnectionStrand_
      , basis::bindFrontOnceClosure(
          base::BindOnce(
            &WsChannel::startAccept<Body, Allocator>
            , base::Unretained(this)
            , base::Passed(base::rvalue_cast(req))
          ))
    );
  }

  /**
  * @brief starts async writing to client
  */
  void sendAsync(
    SharedMessageData message
    , bool is_binary = true) NO_EXCEPTION
    GUARD_METHOD_ON_UNKNOWN_THREAD(send);

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnConnectionStrand(
    const base::Location& from_here
    , CallbackT&& task
    , base::IsNestedPromise isNestedPromise
        = base::IsNestedPromise()) NO_EXCEPTION
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(perConnectionStrand_);

    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task)
      , isNestedPromise);
  }

  // calls `ws_.socket().shutdown`
  void doEof() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

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

  MUST_USE_RETURN_VALUE
  bool isOpen() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

private:
  // Start the asynchronous operation
  // Inits resources before first call to `onAccept()`
  template<class Body, class Allocator>
  void startAccept(
    // Clients sends the HTTP request
    // asking for a WebSocket connection
    UpgradeRequestType<Body, Allocator>&& req) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_)
  {
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(perConnectionStrand_);
    DCHECK_MEMBER_OF_UNKNOWN_THREAD(can_schedule_callbacks_);

    DCHECK(perConnectionStrand_->running_in_this_thread());

    /// \todo Init resources here before first call to `onAccept()`

    // Accept the websocket handshake
    DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
    ws_.async_accept(
      req,
      boost::asio::bind_executor(
        *perConnectionStrand_
        , basis::bindFrontOnceCallback(
            base::BindOnce(
              &WsChannel::onAccept
              , base::Unretained(this)
            ))
      )
    );
  }

  void allocateRecievedDataComponent(
    std::string&& message) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(*netRegistry_);

  /**
  * @brief starts async writing to client
  */
  void send(
    SharedMessageData message
    , bool is_binary = true) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  // Used as callback in `async_write`.
  void onWrite(
    ErrorCode ec
    , std::size_t bytes_transferred) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  /// \note Only one asynchronous operation
  /// can be active at once,
  /// see github.com/boostorg/beast/issues/1092#issuecomment-379119463
  /// It just means that you cannot call
  /// two initiating functions (names that start with async_)
  /// on the same object from different threads simultaneously.
  // So only after `async_write` finished we need can
  // `async_write` again using data scheduled in queue.
  void writeQueued() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  void doRead() NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  // Used as callback in `async_close`.
  void onClose(ErrorCode ec) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  void onAccept(ErrorCode ec) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  void onRead(
    ErrorCode ec
    , std::size_t bytes_transferred) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

  void onFail(
    ErrorCode ec
    , char const* what) NO_EXCEPTION
    PRIVATE_METHOD_RUN_ON(&perConnectionStrand_);

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
  SET_WEAK_POINTERS(WsChannel);

  /**
   * The websocket::stream class template
   * provides asynchronous and blocking message-oriented
   * functionality necessary for clients and servers
   * to utilize the WebSocket protocol.
   * @note all asynchronous operations are performed
   * within the same implicit or explicit strand.
   **/
  boost::beast::websocket::stream<
    // stream with custom rate limiter
    ::boost::beast::limited_tcp_stream
  > ws_
    GUARDED_BY(perConnectionStrand_);

  bool isSendBusy_
    GUARDED_BY(perConnectionStrand_);

  // |stream_| and calls to |async_*| are guarded by strand
  basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(perConnectionStrand_);

  // The dynamic buffer to store recieved data
  MessageBufferType readBuffer_
    GUARDED_BY(perConnectionStrand_);

  // used by |entity_id_|
  util::UnownedRef<ECS::NetworkRegistry> netRegistry_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(netRegistry_);

  // `per-connection entity`
  // i.e. per-connection data storage
  const ECS::Entity entity_id_
    GUARD_MEMBER_OF_UNKNOWN_THREAD(entity_id_);

  /// \todo SSL support
  /// ::boost::asio::ssl::context

  /**
   * We want to send more than one message at a time,
   * so we need to implement your own write queue.
   * @see github.com/boostorg/beast/issues/1207
   **/
  /// \note We use circular buffer, so expect that
  /// OLDEST data will be replaced with NEWEST data
  /// (if circular buffer is full).
  CircularMessageBuffer sendBuffer_
    GUARDED_BY(perConnectionStrand_);

  /// \note Object invalidation split between threads (see `markUnused`),
  /// so we want to prohibit callback execution
  /// while performing object invalidation.
  DEBUG_ATOMIC_FLAG(can_schedule_callbacks_)
    // assumed to be thread-safe
    GUARD_MEMBER_OF_UNKNOWN_THREAD(can_schedule_callbacks_);

  // detect infinite recursion
  FUNCTION_RECURSION_CHECKER_LIMIT_999(doEof);

  // detect infinite recursion
  FUNCTION_RECURSION_CHECKER_LIMIT_999(onFail);

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  /// \note can by called on any thread
  CREATE_METHOD_GUARD(send);

  DISALLOW_COPY_AND_ASSIGN(WsChannel);
};

} // namespace ws
} // namespace flexnet
