#include "flexnet/websocket/listener.hpp" // IWYU pragma: associated
#include "flexnet/ECS/components/tcp_connection.hpp"

#include <base/rvalue_cast.h>
#include <base/bind.h>
#include <base/location.h>
#include <base/logging.h>
#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/guid.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/command_line.h>
#include <base/strings/string_number_conversions.h>

#include <basis/ECS/tags.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/promise/post_promise.h>
#include <basis/core/scoped_cleanup.hpp>
#include <basis/status/status_macros.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp> // IWYU pragma: keep
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>
#include <basis/command_line/command_line_macros.hpp>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context_strand.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl.hpp>

#include <boost/beast/websocket.hpp>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>

ECS_DEFINE_METATYPE(UnusedAcceptResultTag)

ECS_DEFINE_METATYPE(::base::Optional<::flexnet::ws::Listener::AcceptConnectionResult>)

ECS_DEFINE_METATYPE(::base::Optional<::flexnet::ws::Listener::StrandComponent>)

namespace flexnet {
namespace ws {

Listener::Listener(
  IoContext& ioc
  , EndpointType&& endpoint
  , ECS::SafeRegistry& registry
  , EntityAllocatorCb entityAllocator)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , ioc_(REFERENCED(ioc))
  , endpoint_(endpoint)
  , registry_(REFERENCED(registry))
  , acceptorStrand_(
      /// \note `get_executor` returns copy
      ioc.get_executor())
  , acceptor_(ioc)
#if DCHECK_IS_ON()
  , sm_(UNINITIALIZED, fillStateTransitionTable())
#endif // DCHECK_IS_ON()
  , entityAllocator_(entityAllocator)
  , warnBigRegistrySize_(100000)
  , warnBigRegistryFreqMs_(100)
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  GET_SWITCH_AS_INT(&warnBigRegistryFreqMs_, "warn-big-registry-freq-ms");
  GET_SWITCH_AS_SIZE_T(&warnBigRegistrySize_, "warn-big-registry-size");

  FAIL_POINT(onAborted_) = FAIL_POINT_INSTANCE(FP_ConnectionAborted);

  ENABLE_FAIL_POINT_IF_HAS_SWITCH(
    FAIL_POINT(onAborted_), "fp_1_accepted_connection_aborted");
}

void Listener::logFailure(
  const ErrorCode& ec, char const* what)
{
  LOG_CALL(DVLOG(99));

  DCHECK_VALID_PTR(what);

  // NOTE: If you got logFailure: accept: Too many open files
  // set ulimit -n 4096, see stackoverflow.com/a/8583083/10904212
  // Restart the accept operation if we got the connection_aborted error
  // and the enable_connection_aborted socket option is not set.
  if (ec == ::boost::asio::error::connection_aborted)
  {
    LOG_ERROR_CODE(VLOG(99),
                   "Listener failed with"
                   " connection_aborted error: ", what, ec);
    return;
  }

  // ssl::error::stream_truncated, also known as an SSL "short read",
  // indicates the peer closed the connection without performing the
  // required closing handshake (for example, Google does this to
  // improve performance). Generally this can be a security issue,
  // but if your communication protocol is self-terminated (as
  // it is with both HTTP and WebSocket) then you may simply
  // ignore the lack of close_notify.
  //
  // https://github.com/boostorg/beast/issues/38
  //
  // https://security.stackexchange.com/questions/91435/how-to-handle-a-malicious-ssl-tls-shutdown
  //
  // When a short read would cut off the end of an HTTP message,
  // Beast returns the error beast::http::error::partial_message.
  // Therefore, if we see a short read here, it has occurred
  // after the message has been completed, so it is safe to ignore it.
  if(ec == ::boost::asio::ssl::error::stream_truncated)
  {
    LOG_ERROR_CODE(VLOG(99),
                   "Listener failed with"
                   " stream_truncated error: ", what, ec);
    return;
  }

  if (ec == ::boost::asio::error::operation_aborted)
  {
    LOG_ERROR_CODE(VLOG(99),
                   "Listener failed with"
                   " operation_aborted error: ", what, ec);
    return;
  }

  if (ec == ::boost::beast::websocket::error::closed)
  {
    LOG_ERROR_CODE(VLOG(99),
                   "Listener failed with"
                   " websocket closed error: ", what, ec);
    return;
  }

  LOG_ERROR_CODE(VLOG(99),
                 "Listener failed with"
                 " error: ", what, ec);
}

::basis::Status Listener::openAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  ErrorCode ec;

  DCHECK(
    sm_.currentState() == Listener::UNINITIALIZED
    || sm_.currentState() == Listener::PAUSED)
    << " current state:"
    << sm_.currentState();

  VLOG(9)
    << "opening acceptor for "
    << endpoint_.address().to_string();

  acceptor_.open(endpoint_.protocol(), ec);
  if (ec)
  {
    logFailure(ec, "open");
    RETURN_ERROR()
           << "Could not call open for acceptor";
  }

  if(!isAcceptorOpen()) {
    RETURN_ERROR()
           << "Failed to open acceptor";
  }

  RETURN_OK();
}

::basis::Status Listener::configureAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  DCHECK(
    sm_.currentState() == Listener::UNINITIALIZED
    || sm_.currentState() == Listener::PAUSED)
    << " current state:"
    << sm_.currentState();

  ErrorCode ec;

  // @see boost.org/doc/libs/1_61_0/doc/html/boost_asio/reference/basic_socket.html
  // Allow address reuse
  // NOTE: In windows, the option tcp::acceptor::reuse_address
  // is equivalent to calling setsockopt and specifying SO_REUSEADDR.
  // This specifically allows multiple sockets to be bound
  // to an address even if it is in use.
  // @see stackoverflow.com/a/7195105/10904212
  acceptor_.set_option(
    ::boost::asio::socket_base::reuse_address(true)
    , ec);
  if (ec)
  {
    logFailure(ec, "set_option");
    RETURN_ERROR()
           << "Could not call set_option for acceptor";
  }

  // Bind to the server address
  acceptor_.bind(endpoint_, ec);
  if (ec)
  {
    logFailure(ec, "bind");
    RETURN_ERROR()
           << "Could not call bind for acceptor";
  }

  VLOG(9)
    << "acceptor listening endpoint: "
    << endpoint_.address().to_string();

  acceptor_.listen(
    ::boost::asio::socket_base::max_listen_connections
    , ec);
  if (ec)
  {
    logFailure(ec, "listen");
    RETURN_ERROR()
           << "Could not call listen for acceptor";
  }

  RETURN_OK();
}

Listener::StatusPromise Listener::configureAndRun()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(!isAcceptingInThisThread());
  /// \note `configure` is not hot code path,
  /// so it is ok to use `base::Promise` here
  return postTaskOnAcceptorStrand(
    FROM_HERE
    , ::base::bindCheckedOnce(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &Listener::configureAndRunAcceptor
        , ::base::Unretained(this)));
}

::basis::Status Listener::configureAndRunAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  DCHECK(
    sm_.currentState() == Listener::UNINITIALIZED
    || sm_.currentState() == Listener::PAUSED)
    << " current state:"
    << sm_.currentState();

  RETURN_IF_NOT_OK(
    openAcceptor());

  RETURN_IF_NOT_OK(
    configureAcceptor());

  if(!isAcceptorOpen())
  {
    RETURN_ERROR()
           << "Unable to run closed acceptor";
  }

#if DCHECK_IS_ON()
  ignore_result(
    processStateChange(FROM_HERE, START));
#endif // DCHECK_IS_ON()

  doAccept();

  /// \todo always ok
  RETURN_OK();
}

void Listener::doAccept()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  // prevent infinite recursion
  if(!isAcceptorOpen())
  {
    DVLOG(9)
      << "accept recursion stopped"
      << " because acceptor is not open";
    return;
  }

  DCHECK(
    sm_.currentState() == Listener::STARTED)
    << " current state:"
    << sm_.currentState();

  /// \note resources will be preallocated
  /// BEFORE anyone connected
  /// (before callback of `async_accept`)

  registry_.taskRunner()->PostTask(
    FROM_HERE
    , ::base::bindCheckedOnce(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &Listener::allocateTcpResourceAndAccept
        , ::base::Unretained(this))
  );
}

void Listener::allocateTcpResourceAndAccept()
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(acceptorStrand_);

  DCHECK(registry_.RunsTasksInCurrentSequence());

  LOG_IF_EVERY_N_MS(WARNING
                    , registry_->size() > warnBigRegistrySize_
                    , warnBigRegistryFreqMs_)
   << "Asio registry has more than "
   << warnBigRegistrySize_
   << " entities."
   << " That may signal about some problem in code"
   << " or DDOS."
   << " Make sure registry stores only"
   << " entities used by network connections"
   << " and unused entities can be freed"
      " with proper frequency.";

  DCHECK(entityAllocator_);
  ECS::Entity tcp_entity_id
    = entityAllocator_.Run();
  DCHECK(registry_->valid(tcp_entity_id));

  DCHECK(registry_->has<ECS::TcpConnection>(tcp_entity_id));
  ECS::TcpConnection& tcpComponent
    = registry_->get<ECS::TcpConnection>(tcp_entity_id);

  DVLOG(99)
    << "using TcpConnection with id: "
    << tcpComponent.debug_id;

  StrandComponent* asioStrandCtx
    = &tcpComponent->reset_or_create_var<StrandComponent>(
        "Ctx_StrandComponent_" + ::base::GenerateGUID() // debug name
        /// \note `get_executor` returns copy
        , ioc_.get_executor());

  // Check that if the value already existed
  // it was overwritten
  // Also we expect that all allocated strands
  // have same io context executor
  DCHECK(asioStrandCtx->value().get_inner_executor()
    /// \note `get_executor` returns copy
    == ioc_.get_executor());

  if(ioc_.stopped())
  {
    LOG_CALL(LOG(WARNING))
      << "unable to `::boost::asio::post` on stopped ioc";
    NOTREACHED();
    return;
  }

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  // Accept connection
  ::boost::asio::post(
    *acceptorStrand_
    , ::basis::bindFrontOnceCallback(
        ::base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &Listener::asyncAccept
          , ::base::Unretained(this)
          , COPIED(
              ::basis::UnownedPtr<StrandType>(&asioStrandCtx->value()))
          , COPIED(tcp_entity_id)))
  );
}

void Listener::asyncAccept(
  ::basis::UnownedPtr<StrandType> perConnectionStrand
  , ECS::Entity tcp_entity_id)
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  if(!isAcceptorOpen())
  {
    LOG(WARNING)
      << "unable to accept new connection:"
         " acceptor not open";
    return;
  }

  DCHECK(
    sm_.currentState() == Listener::STARTED)
    << " current state:"
    << sm_.currentState();

  DCHECK(perConnectionStrand);

  /// Start an asynchronous accept.
  /**
   * This function is used to asynchronously accept a new connection. The
   * function call always returns immediately.
   */
  acceptor_.async_accept(
    /**
     * I/O objects such as sockets and streams are not thread-safe.
     * For efficiency, networking adopts
     * a model of using threads without explicit locking
     * by requiring all access to I/O objects to be
     * performed within a strand.
     */
    // new connection needs its own strand
    // created socket will be associated with strand
    // that was passed to |async_accept|
    *perConnectionStrand.Get()
    , boost::asio::bind_executor(
        *perConnectionStrand.Get()
        , ::basis::bindFrontOnceCallback(
            ::base::bindCheckedOnce(
              DEBUG_BIND_CHECKS(
                PTR_CHECKER(this)
              )
              , &Listener::onAccept
              , ::base::Unretained(this)
              , COPIED(perConnectionStrand)
              , COPIED(tcp_entity_id)))
      )
    );
}

::basis::Status Listener::pause()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

#if DCHECK_IS_ON()
  ignore_result(
    processStateChange(FROM_HERE, PAUSE));
#endif // DCHECK_IS_ON()

  /// \todo IMPLEMENT server pause
  NOTIMPLEMENTED();

  RETURN_OK();
}

::basis::Status Listener::stopAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

#if DCHECK_IS_ON()
  ignore_result(
    processStateChange(FROM_HERE, TERMINATE));
#endif // DCHECK_IS_ON()

  /// \note we usually post `stopAcceptor()` using `boost::asio::post`,
  /// so need to check if acceptor was closed during delay
  /// added by `boost::asio::post`
  if(!isAcceptorOpen())
  {
    VLOG(9)
      << "unable to stop closed listener";

    RETURN_OK();
  }

  ErrorCode ec;

  if (isAcceptorOpen())
  {
    VLOG(9)
      << "closing acceptor...";

    acceptor_.cancel(ec);
    if (ec)
    {
      logFailure(ec, "acceptor_cancel");
      RETURN_ERROR()
             << "Failed to call acceptor_cancel for acceptor";
    }

    /// \note does not close alive sessions, just
    /// stops accepting incoming connections
    /// \note stopped acceptor may be continued via `async_accept`
    acceptor_.close(ec);
    if (ec) {
      {
        logFailure(ec, "acceptor_close");
      }

      RETURN_ERROR()
             << "Failed to call acceptor_close for acceptor";
    }

    // acceptor must be closed here without errors
    DCHECK(!isAcceptorOpen());
  }

  RETURN_OK();
}

#if DCHECK_IS_ON()
basis::Status Listener::processStateChange(
  const ::base::Location &from_here
  , const Listener::Event &processEvent)
{
  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  const ::basis::Status stateProcessed
      = sm_.ProcessEvent(processEvent
                         , FROM_HERE.ToString()
                         , nullptr);
  if(!stateProcessed.ok())
  {
    LOG(WARNING)
      << "Failed to change state"
      << " using event "
      << processEvent
      << " in code "
      << from_here.ToString()
      << ". Current state: "
      << sm_.currentState();
    DCHECK(false);
  }
  return stateProcessed;
}
#endif // DCHECK_IS_ON()

Listener::StatusPromise Listener::stopAcceptorAsync()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_)
    << "use Listener::stopAcceptor()";

  /// \note we asume that no more tasks
  /// will be posted on |acceptorStrand_|
  /// and it is safe to destruct |Listener|
  /// (or unpause i.e. re-open it again)
  DCHECK(!isAcceptingInThisThread());
  /// \note `stop` is not hot code path,
  /// so it is ok to use `base::Promise` here
  return postTaskOnAcceptorStrand(
        FROM_HERE
        , ::base::bindCheckedOnce(
            DEBUG_BIND_CHECKS(
              PTR_CHECKER(this)
            )
            , &Listener::stopAcceptor,
            ::base::Unretained(this)));
}

void Listener::onAccept(basis::UnownedPtr<StrandType> perConnectionStrand
                        , ECS::Entity tcp_entity_id
                        , const ErrorCode& ec
                        , SocketType&& socket)
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(acceptorStrand_);

  /// \note may be same or not same as |isAcceptingInThisThread()|
  DCHECK(perConnectionStrand
         && perConnectionStrand->running_in_this_thread());

  ErrorCode errorCode = ec;
  if(UNLIKELY(FAIL_POINT(onAborted_)->checkFail()))
  {
    errorCode = ::boost::asio::error::connection_aborted;
    // `setAcceptConnectionResult` must free ECS::Entity, socket, etc.
  }

  if (errorCode)
  {
    logFailure(errorCode, "accept");
  }

  if (!socket.is_open())
  {
    VLOG(9)
      << "accepted connection"
         " has closed socket";
    /// \note do not forget to free allocated resources
    /// i.e. handle error code
  } else {
    DCHECK(socket.is_open());
    const EndpointType& remote_endpoint
      /// \note Transport endpoint must be connected i.e. `is_open()`
      = socket.remote_endpoint();
    VLOG(9)
        << "Listener accepted remote endpoint: "
        << remote_endpoint;
  }

  if(ioc_.stopped())
  {
    LOG_CALL(LOG(WARNING))
      << "unable to `::boost::asio::post` on stopped ioc";
    NOTREACHED();
    return;
  }

  // mark connection as newly created
  // (or as failed with error code)

  registry_.taskRunner()->PostTask(
    FROM_HERE
    , ::base::bindCheckedOnce(
        DEBUG_BIND_CHECKS(
          PTR_CHECKER(this)
        )
        , &Listener::setAcceptConnectionResult
        , ::base::Unretained(this)
        , COPIED(tcp_entity_id)
        , COPY_OR_MOVE(errorCode)
        , RVALUE_CAST(socket)
      )
  );

  // Accept another connection
  ::boost::asio::post(
    *acceptorStrand_
    , ::basis::bindFrontOnceCallback(
        ::base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &Listener::doAccept
          , ::base::Unretained(this)))
  );
}

void Listener::setAcceptConnectionResult(
  ECS::Entity tcp_entity_id
  , ErrorCode&& ec
  , SocketType&& socket)
{
  DCHECK(registry_.RunsTasksInCurrentSequence());

  DCHECK(registry_->valid(tcp_entity_id));

  DVLOG(99)
    << " added new connection";

  ECS::TcpConnection& tcpComponent
    = registry_->get<ECS::TcpConnection>(tcp_entity_id);

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  {
    using UniqueAcceptComponent
      = ::base::Optional<Listener::AcceptConnectionResult>;

    // If the value already exists allow it to be re-used
    registry_->remove_if_exists<
        ECS::UnusedAcceptResultTag
      >(tcp_entity_id);

    // destroy TCP Entity if socket is closed
    /// \note we do not force closing of connection in case of `ec`
    /// because user may want to skip some error codes.
    bool forceClosing
      = !socket.is_open();

    DVLOG_IF(99, forceClosing)
      << " forcing close of connection";

    UniqueAcceptComponent& acceptResult
      = registry_.reset_or_create_component<UniqueAcceptComponent>(
            "UniqueAcceptComponent_" + ::base::GenerateGUID() // debug name
            , tcp_entity_id
            , RVALUE_CAST(ec)
            , RVALUE_CAST(socket)
            , forceClosing);
  }
}

Listener::~Listener()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG_CALL(DVLOG(99));

  DCHECK_UNOWNED_REF(ioc_);
  DCHECK_UNOWNED_REF(registry_);

  /// \note we assume that |is_open|
  /// is thread-safe in destructor
  /// (but not thread-safe in general)
  /// i.e. do not modify acceptor
  /// from any thread if you reached destructor
  DCHECK(!acceptor_.is_open());

  /// \note we expect that API user will call
  /// `close` for acceptor before acceptor destructon
  DCHECK(
    sm_.currentState() == Listener::UNINITIALIZED
    || sm_.currentState() == Listener::TERMINATED
    || sm_.currentState() == Listener::FAILED)
    << " current state:"
    << sm_.currentState();

  // make sure that all allocated
  // `per-connection resources` are freed
  // i.e. use check `registry.empty()`
  {
    /// \note (thread-safety) access from destructor when ioc->stopped
    /// i.e. assume no running asio threads that use |registry_|
    DCHECK(registry_.registryUnsafe().empty());
  }

  /// \note Callbacks posted on |io_context| can use |this|,
  /// so make sure that |this| outlives |io_context|
  /// (callbacks expected to NOT execute on stopped |io_context|).
  DCHECK(ioc_.stopped());

  DVLOG(99)
    << "asio acceptor was freed";
}

bool Listener::isAcceptorOpen() const
{
  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  /// \note |is_open| is not thread-safe in general
  /// i.e. provide thread-safety checks
  return acceptor_.is_open();
}

bool Listener::isAcceptingInThisThread() const NO_EXCEPTION
{
  DCHECK_MEMBER_GUARD(acceptorStrand_);

  /// \note `running_in_this_thread()` assumed to be thread-safe
  return acceptorStrand_->running_in_this_thread();
}

} // namespace ws
} // namespace flexnet
