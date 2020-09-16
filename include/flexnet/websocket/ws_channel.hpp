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

#include <basis/checked_optional.hpp>
#include <basis/lock_with_check.hpp>
#include <basis/ECS/asio_registry.hpp>
#include <basis/promise/post_promise.h>
#include <basis/status/statusor.hpp>
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
// Use plain collbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
class WsChannel
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

  struct QueuedMessage {
    /// \todo remove shared_ptr
    std::shared_ptr<const std::string> data;
    bool is_binary;
  };

public:
  WsChannel(
    // Take ownership of the stream
    StreamType&& stream
    , ECS::AsioRegistry& asioRegistry
    , const ECS::Entity entity_id);

  WsChannel(
    WsChannel&& other) = delete;

  /// \note can destruct on any thread
  ~WsChannel()
    RUN_ON_ANY_THREAD_LOCKS_EXCLUDED(fn_WsChannelDestructor);

  // Start the asynchronous operation
  template<class Body, class Allocator>
  void startAccept(
    // Clients sends the HTTP request
    // asking for a WebSocket connection
    UpgradeRequestType<Body, Allocator>&& req)
    RUN_ON(&perConnectionStrand_)
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_perConnectionStrand_);

    DCHECK(perConnectionStrand_->running_in_this_thread());

    // Accept the websocket handshake
    ws_.async_accept(
      req,
      /// \todo use base::BindFrontWrapper
      /*::boost::beast::bind_front_handler([
        ](
          base::OnceCallback<void(ErrorCode)>&& task
          , ErrorCode ec
        ){
          DCHECK(task);
          base::rvalue_cast(task).Run(ec);
        }
        , base::BindOnce(
          &WsChannel::onAccept
          , UNOWNED_LIFETIME(base::Unretained(this))
        )
      )*/
      boost::asio::bind_executor(
        *perConnectionStrand_
        , ::std::bind(
            &WsChannel::onAccept,
            UNOWNED_LIFETIME(
              this)
            , std::placeholders::_1
          )
      )
    );
  }

  // Start the asynchronous operation
  template<class Body, class Allocator>
  void startAcceptAsync(
    // Clients sends the HTTP request
    // asking for a WebSocket connection
    UpgradeRequestType<Body, Allocator>&& req)
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_perConnectionStrand_);

    ::boost::asio::post(
      *perConnectionStrand_
      /// \todo use base::BindFrontWrapper
      , ::boost::beast::bind_front_handler([
          ](
            base::OnceClosure&& task
          ){
            DCHECK(task);
            base::rvalue_cast(task).Run();
          }
          , base::BindOnce(
            &WsChannel::startAccept<Body, Allocator>
            , UNOWNED_LIFETIME(base::Unretained(this))
            , base::Passed(base::rvalue_cast(req))
          )
        )
      /*, boost::asio::bind_executor(
        *perConnectionStrand_
        , ::std::bind(
            &WsChannel::startAccept<Body, Allocator>,
            UNOWNED_LIFETIME(
              this)
            , base::rvalue_cast(req)
          )
      )*/
    );
  }

  void doRead()
    RUN_ON(&perConnectionStrand_);

  void onClose(ErrorCode ec)
    RUN_ON(&perConnectionStrand_);

  MUST_USE_RETURN_VALUE
  bool isOpen()
    RUN_ON(&perConnectionStrand_);

#if 0
  /**
  * @brief starts async writing to client
  */
  void sendMessageAsync(
    const std::string& message
    , bool is_binary = true) override;

  /**
  * @brief starts async writing to client
  */
  void sendMessage(
    const std::string& message
    , bool is_binary = true) override;

  void doReadAsync();

  void handleWebsocketUpgrade(
    ErrorCode ec
    , std::size_t bytes_transferred);

  /// \note `stream_` can be moved to websocket session from http session,
  /// so we can't use it here anymore
  MUST_USE_RETURN_VALUE
  bool isStreamValid() const
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_is_stream_valid_);
    return is_stream_valid_.load();
  }
#endif // 0

  template <typename CallbackT>
  MUST_USE_RETURN_VALUE
  auto postTaskOnConnectionStrand(
    const base::Location& from_here
    , CallbackT&& task
    , bool nestedPromise = false)
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_perConnectionStrand_);

    return base::PostPromiseOnAsioExecutor(
      from_here
      // Post our work to the strand, to prevent data race
      , *perConnectionStrand_
      , std::forward<CallbackT>(task)
      , nestedPromise);
  }

  // calls `ws_.socket().shutdown`
  void doEof()
    RUN_ON(&perConnectionStrand_);

  /// \note returns COPY because of thread safety reasons:
  /// `entity_id_` assumed to be NOT changed,
  /// so its copy can be read from any thread.
  /// `ECS::Entity` is just number, so can be copied freely.
  MUST_USE_RETURN_VALUE
  ECS::Entity entityId() const
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_entity_id_);
    return entity_id_;
  }

  MUST_USE_RETURN_VALUE
  base::WeakPtr<WsChannel> weakSelf() const NO_EXCEPTION
  {
    DCHECK_CUSTOM_THREAD_GUARD(guard_weak_this_);

    // It is thread-safe to copy |base::WeakPtr|.
    // Weak pointers may be passed safely between sequences, but must always be
    // dereferenced and invalidated on the same SequencedTaskRunner otherwise
    // checking the pointer would be racey.
    return weak_this_;
  }

private:
  void onAccept(ErrorCode ec)
    RUN_ON(&perConnectionStrand_);

  void onRead(ErrorCode ec, std::size_t bytes_transferred)
    RUN_ON(&perConnectionStrand_);

#if 0

  void onClose(
    ErrorCode ec);

  void onWrite(
    ErrorCode ec
    , std::size_t bytes_transferred);

  void onPing(
    ErrorCode ec);
#endif // 0

  void onFail(
    ErrorCode ec
    , char const* what)
    RUN_ON(&perConnectionStrand_);

private:

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
    SET_CUSTOM_THREAD_GUARD(guard_perConnectionStrand_);

  // The dynamic buffer to store recieved data
  MessageBufferType buffer_
    GUARDED_BY(perConnectionStrand_);

  // base::WeakPtr can be used to ensure that any callback bound
  // to an object is canceled when that object is destroyed
  // (guarantees that |this| will not be used-after-free).
  base::WeakPtrFactory<WsChannel> weak_ptr_factory_
    GUARDED_BY(sequence_checker_);

  // After constructing |weak_ptr_factory_|
  // we immediately construct a WeakPtr
  // in order to bind the WeakPtr object to its thread.
  // When we need a WeakPtr, we copy construct this,
  // which is safe to do from any
  // thread according to weak_ptr.h (versus calling
  // |weak_ptr_factory_.GetWeakPtr() which is not).
  const base::WeakPtr<WsChannel> weak_this_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(guard_weak_this_);

  // used by |entity_id_|
  util::UnownedRef<ECS::AsioRegistry> asioRegistry_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(guard_asioRegistry_);

  // `per-connection entity`
  // i.e. per-connection data storage
  const ECS::Entity entity_id_
    // It safe to read value from any thread because its storage
    // expected to be not modified (if properly initialized)
    SET_CUSTOM_THREAD_GUARD(guard_entity_id_);

  /// \todo SSL support
  /// ::boost::asio::ssl::context

  /**
   * We want to send more than one message at a time,
   * so we need to implement your own write queue.
   * @see github.com/boostorg/beast/issues/1207
   **/
  std::vector<std::shared_ptr<QueuedMessage>> sendQueue_;

  // check sequence on which class was constructed/destructed/configured
  SEQUENCE_CHECKER(sequence_checker_);

  /// \note can destruct on any thread
  CREATE_CUSTOM_THREAD_GUARD(fn_WsChannelDestructor);

  DISALLOW_COPY_AND_ASSIGN(WsChannel);
};

} // namespace ws
} // namespace flexnet
