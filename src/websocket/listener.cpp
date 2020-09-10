#include "flexnet/websocket/listener.hpp" // IWYU pragma: associated

#include "flexnet/ECS/tags.hpp"
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
#include <basis/promise/post_promise.h>
#include <basis/scoped_cleanup.hpp>
#include <basis/status/status_macros.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp> // IWYU pragma: keep

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

namespace flexnet {
namespace ws {

Listener::Listener(
  IoContext& ioc
  , EndpointType&& endpoint
  , ECS::AsioRegistry& asioRegistry)
  : ioc_(REFERENCED(ioc))
  , endpoint_(endpoint)
  , asioRegistry_(REFERENCED(asioRegistry))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , acceptorStrand_(ioc.get_executor())
  , acceptor_(ioc)
#if DCHECK_IS_ON()
  , sm_(UNINITIALIZED, FillStateTransitionTable())
#endif // DCHECK_IS_ON()
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

NOT_THREAD_SAFE_FUNCTION()
void Listener::logFailure(
  const ErrorCode& ec, char const* what)
{
  LOG_CALL(DVLOG(99));

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

::util::Status Listener::openAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  ErrorCode ec;

  DCHECK(
    sm_.CurrentState() == Listener::UNINITIALIZED
    || sm_.CurrentState() == Listener::PAUSED);

  VLOG(9)
    << "opening acceptor for "
    << endpoint_.address().to_string();

  acceptor_.open(endpoint_.protocol(), ec);
  if (ec)
  {
    logFailure(ec, "open");
    return MAKE_ERROR()
           << "Could not call open for acceptor";
  }

  if(!isAcceptorOpen()) {
    return MAKE_ERROR()
           << "Failed to open acceptor";
  }

  return ::util::OkStatus();
}

::util::Status Listener::configureAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  DCHECK(
    sm_.CurrentState() == Listener::UNINITIALIZED
    || sm_.CurrentState() == Listener::PAUSED);

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
    return MAKE_ERROR()
           << "Could not call set_option for acceptor";
  }

  // Bind to the server address
  acceptor_.bind(endpoint_, ec);
  if (ec)
  {
    logFailure(ec, "bind");
    return MAKE_ERROR()
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
    return MAKE_ERROR()
           << "Could not call listen for acceptor";
  }

  return ::util::OkStatus();
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
    , base::BindOnce(
      &Listener::
        configureAndRunAcceptor,
      base::Unretained(this)));
}

::util::Status Listener::configureAndRunAcceptor()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  DCHECK(
    sm_.CurrentState() == Listener::UNINITIALIZED
    || sm_.CurrentState() == Listener::PAUSED);

  RETURN_IF_ERROR(
    openAcceptor());

  RETURN_IF_ERROR(
    configureAcceptor());

  if(!isAcceptorOpen())
  {
    return MAKE_ERROR()
           << "Unable to run closed acceptor";
  }

#if DCHECK_IS_ON()
  ignore_result(
    processStateChange(FROM_HERE, START));
#endif // DCHECK_IS_ON()

  doAccept();

  /// \todo always ok
  return ::util::OkStatus();
}

void Listener::doAccept()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

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
    sm_.CurrentState() == Listener::STARTED);

  /// \note resources will be preallocated
  /// BEFORE anyone connected
  /// (before callback of `async_accept`)
  ::boost::asio::post(
    asioRegistry_->strand()
    , ::std::bind(
        &Listener::allocateTcpResourceAndAccept
        , this)
  );
}

void Listener::allocateTcpResourceAndAccept()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(acceptorStrand_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  DCHECK(asioRegistry_->running_in_this_thread());

  const size_t kWarnBigRegistrySize = 100000;
  LOG_IF(WARNING
    , (*asioRegistry_)->size() > kWarnBigRegistrySize)
   << "Asio registry has more than "
   << kWarnBigRegistrySize
   << " entities."
   << " That may signal about some problem in code"
   << " or DDOS."
   << " Make sure registry stores only"
   << " entities used by network connections"
   << " and unused entities can be freed"
      " with proper frequency.";

  ECS::Entity tcp_entity_id
    = createTcpEntity();
  DCHECK((*asioRegistry_)->valid(tcp_entity_id));

  /// \note `tcpComponent.context.empty()` may be false
  /// if unused `tcpComponent` found using `registry.get`
  /// i.e. we do not fully reset `tcpComponent`,
  /// so reset each type stored in context individually.
  ECS::TcpConnection& tcpComponent
    = (*asioRegistry_)->get_or_emplace<ECS::TcpConnection>(
        tcp_entity_id
        , "TcpConnection_" + base::GenerateGUID() // debug name
      );

  DVLOG(99)
    << "using TcpConnection with id: "
    << (*asioRegistry_)->get<ECS::TcpConnection>(tcp_entity_id).debug_id;

  const bool useCache
    = tcpComponent->try_ctx_var<StrandComponent>();

  DVLOG(99)
    << (useCache
        ? "using preallocated strand"
        : "allocating new strand");

  StrandComponent* asioStrandCtx
    = &tcpComponent->ctx_or_set_var<StrandComponent>(
        "Ctx_StrandComponent_" + base::GenerateGUID() // debug name
        , ioc_->get_executor());

  // If the value already exists it is overwritten
  if(useCache) {
    asioStrandCtx = &tcpComponent->set_var<StrandComponent>(
      "Ctx_StrandComponent_" + base::GenerateGUID() // debug name
      , ioc_->get_executor());
  }

  // Check that if the value already existed
  // it was overwritten
  // Also we expect that all allocated strands
  // have same io context executor
  DCHECK(asioStrandCtx->get_inner_executor()
       == ioc_->get_executor());

  // unable to `::boost::asio::post` on stopped ioc
  DCHECK(!ioc_->stopped());

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  // Accept connection
  ::boost::asio::post(
    *acceptorStrand_
    , ::std::bind(
        &Listener::asyncAccept
        , this
        , COPIED(
            util::UnownedPtr<StrandType>(asioStrandCtx))
        , COPIED(tcp_entity_id))
  );
}

ECS::Entity Listener::createTcpEntity()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

  DCHECK(asioRegistry_->running_in_this_thread());

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    /// \todo use `entt::group` instead of `entt::view` here
    /// if it will speed things up
    /// i.e. measure perf. and compare results
    = (*asioRegistry_)->view<ECS::TcpConnection, ECS::UnusedTag>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
        >
      );

  ECS::Entity tcp_entity_id
    = ECS::NULL_ENTITY;

  if(registry_group.empty())
  {
    tcp_entity_id = (*asioRegistry_)->create();
    DVLOG(99)
      << "allocating new network entity";
    DCHECK((*asioRegistry_)->valid(tcp_entity_id));
    return tcp_entity_id;
  }

  // reuse any unused entity (if found)
  for(const ECS::Entity& entity_id : registry_group)
  {
    if(!(*asioRegistry_)->valid(entity_id)) {
      // skip invalid entities
      continue;
    }
    tcp_entity_id = entity_id;
    // need only first valid entity
    break;
  }

  /// \note may not find valid entities,
  /// so checks for `ECS::NULL_ENTITY` using `registry.valid`
  if((*asioRegistry_)->valid(tcp_entity_id)) {
    (*asioRegistry_)->remove<ECS::UnusedTag>(tcp_entity_id);
    DVLOG(99)
      << "using preallocated network entity with id: "
      << (*asioRegistry_)->get<ECS::TcpConnection>(tcp_entity_id).debug_id;
  } else {
    tcp_entity_id = (*asioRegistry_)->create();
    DVLOG(99)
      << "allocating new network entity";
  }

  DCHECK((*asioRegistry_)->valid(tcp_entity_id));

  return tcp_entity_id;
}

void Listener::asyncAccept(
  util::UnownedPtr<StrandType> unownedPerConnectionStrand
  , ECS::Entity tcp_entity_id)
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);
  DCHECK(isAcceptorOpen());

  DCHECK(
    sm_.CurrentState() == Listener::STARTED);

  DCHECK(unownedPerConnectionStrand);

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
    *unownedPerConnectionStrand.Get()
    , boost::asio::bind_executor(
        *unownedPerConnectionStrand.Get()
        , ::std::bind(
          &Listener::onAccept,
          UNOWNED_LIFETIME(
            this)
          , COPIED(unownedPerConnectionStrand)
          , COPIED(tcp_entity_id)
          , std::placeholders::_1
          , std::placeholders::_2
          )
      )
    );
}

::util::Status Listener::pause()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

#if DCHECK_IS_ON()
  ignore_result(
    processStateChange(FROM_HERE, PAUSE));
#endif // DCHECK_IS_ON()

  /// \todo IMPLEMENT
  NOTIMPLEMENTED();

  return ::util::OkStatus();
}

::util::Status Listener::stopAcceptor()
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

    return ::util::OkStatus();
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

      return MAKE_ERROR()
             << "Failed to call acceptor_cancel for acceptor";
    }

    /// \note does not close alive sessions, just
    /// stops accepting incoming connections
    /// \note stopped acceptor may be continued via `async_accept`
    acceptor_.close(ec);
    if (ec) {
      logFailure(ec, "acceptor_close");

      return MAKE_ERROR()
             << "Failed to call acceptor_close for acceptor";
    }

    // acceptor must be closed here without errors
    DCHECK(!isAcceptorOpen());
  }

  return ::util::OkStatus();
}

util::Status Listener::processStateChange(const base::Location &from_here, const Listener::Event &processEvent)
{
  DCHECK_RUN_ON_STRAND(&acceptorStrand_, ExecutorType);

  const ::util::Status stateProcessed
      = sm_.ProcessEvent(processEvent
                         , FROM_HERE.ToString()
                         , nullptr);
  CHECK(stateProcessed.ok())
      << "Failed to change state"
      << " using event "
      << processEvent
      << " in code "
      << from_here.ToString()
      << ". Current state: "
      << sm_.CurrentState();
  return stateProcessed;
}

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
        , base::BindOnce(
          &Listener::stopAcceptor,
          base::Unretained(this)));
}

void Listener::onAccept(util::UnownedPtr<StrandType> unownedPerConnectionStrand
                        , ECS::Entity tcp_entity_id
                        , const ErrorCode& ec
                        , SocketType&& socket)
{
  LOG_CALL(DVLOG(99));

  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);
  DCHECK_CUSTOM_THREAD_GUARD(acceptorStrand_);
  DCHECK_CUSTOM_THREAD_GUARD(ioc_);

  /// \note may be same or not same as |isAcceptingInThisThread()|
  DCHECK(unownedPerConnectionStrand
         && unownedPerConnectionStrand->running_in_this_thread());

  if (ec)
  {
    logFailure(ec, "accept");
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
      = socket.remote_endpoint();
    VLOG(9)
        << "Listener accepted remote endpoint: "
        << remote_endpoint;
  }

  // mark connection as newly created
  // (or as failed with error code)
  ::boost::asio::post(
    asioRegistry_->strand()
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& boundTask
      ){
        base::rvalue_cast(boundTask).Run();
      }
      , base::BindOnce(
          &Listener::setAcceptConnectionResult
          , base::Unretained(this)
          , COPIED(tcp_entity_id)
          , CAN_COPY_ON_MOVE("moving const") std::move(ec)
          , base::rvalue_cast(socket)
        )
    )
  );

  // unable to `::boost::asio::post` on stopped ioc
  DCHECK(!ioc_->stopped());

  // Accept another connection
  ::boost::asio::post(
    *acceptorStrand_
    , ::std::bind(
        &Listener::doAccept
        , this)
  );
}

void Listener::setAcceptConnectionResult(
  ECS::Entity tcp_entity_id
  , ErrorCode&& ec
  , SocketType&& socket)
{
  DCHECK_CUSTOM_THREAD_GUARD(asioRegistry_);

  DCHECK(asioRegistry_->running_in_this_thread());

  DVLOG(99)
    << " added new connection";

  ECS::TcpConnection& tcpComponent
    = (*asioRegistry_)->get<ECS::TcpConnection>(tcp_entity_id);

  // `ECS::TcpConnection` must be valid
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());

  /// \todo create utility function `cached_emplace`
  {
    using UniqueAcceptComponent
      = base::Optional<Listener::AcceptConnectionResult>;

    const bool useCache
      = (*asioRegistry_)->has<UniqueAcceptComponent>(tcp_entity_id);

    (*asioRegistry_)->remove_if_exists<
      ECS::UnusedAcceptResultTag
    >(tcp_entity_id);

    UniqueAcceptComponent& acceptResult
      = useCache
        /// \todo use get_or_emplace
        ? (*asioRegistry_)->get<UniqueAcceptComponent>(tcp_entity_id)
        : (*asioRegistry_)->emplace<UniqueAcceptComponent>(
            tcp_entity_id
            , base::in_place
            , base::rvalue_cast(ec)
            , base::rvalue_cast(socket)
            /// \todo make us of it
            , /* force closing */ false);

    if(useCache) {
      DCHECK(acceptResult);
      acceptResult.emplace(
        base::rvalue_cast(ec)
        , base::rvalue_cast(socket)
        /// \todo make us of it
        , /* force closing */ false);
      DVLOG(99)
        << "using preallocated AcceptConnectionResult";
    } else {
      DVLOG(99)
        << "allocating new AcceptConnectionResult";
    }
  }
}

Listener::~Listener()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG_CALL(DVLOG(99));

  /// \note we assume that |is_open|
  /// is thread-safe in destructor
  /// (but not thread-safe in general)
  /// i.e. do not modify acceptor
  /// from any thread if you reached destructor
  DCHECK(!acceptor_.is_open());

  /// \note we expect that API user will call
  /// `close` for acceptor before acceptor destructon
  DCHECK(
    sm_.CurrentState() == Listener::UNINITIALIZED
    || sm_.CurrentState() == Listener::TERMINATED);

  // make sure that all allocated
  // `per-connection resources` are freed
  // i.e. use check `registry.empty()`
  DCHECK(asioRegistry_
    ->registry_unsafe(FROM_HERE
      , "access from destructor when ioc->stopped"
        " i.e. no running asio threads that use |asioRegistry_|"
      , base::BindOnce([](const util::UnownedRef<IoContext>& ioc)
        {
          // checks that access to |asioRegistry_| is thread-safe
          DCHECK(ioc->stopped());
        }
        , CONST_REFERENCED(ioc_)))
    .empty());

  /// \note Callbacks posted on |io_context| can use |this|,
  /// so make sure that |this| outlives |io_context|
  /// (callbacks expected to NOT execute on stopped |io_context|).
  DCHECK(ioc_->stopped());

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
  DCHECK_CUSTOM_THREAD_GUARD(acceptorStrand_);

  /// \note `running_in_this_thread()` assumed to be thread-safe
  return acceptorStrand_->running_in_this_thread();
}

} // namespace ws
} // namespace flexnet
