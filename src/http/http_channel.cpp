#include "flexnet/http/http_channel.hpp" // IWYU pragma: associated
#include "flexnet/util/mime_type.hpp"
#include "flexnet/websocket/ws_channel.hpp"
#include "flexnet/ECS/components/tcp_connection.hpp"
#include "flexnet/util/close_socket_unsafe.hpp"

#include <base/rvalue_cast.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/threading/thread.h>
#include <base/guid.h>
#include <base/task/thread_pool/thread_pool.h>

#include <basis/ECS/tags.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/task_util.hpp>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/assert.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/basic_stream_socket.hpp>
#include <boost/asio/bind_executor.hpp>

#include <algorithm>
#include <ratio>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace beast = ::boost::beast;

ECS_DEFINE_METATYPE(::base::Optional<flexnet::http::HttpChannel>)

namespace {

using HttpChannel
  = flexnet::http::HttpChannel;

template<class Allocator>
using basic_fields
  = beast::http::basic_fields<Allocator>;

/// \todo use ::base::FilePath
// Append an HTTP rel-path to a local filesystem path.
// The returned path is normalized for the platform.
std::string
pathConcat(
  beast::string_view base,
  beast::string_view path)
{
  if(base.empty()) {
    return std::string(path);
  }

  std::string result(base);
#ifdef BOOST_MSVC
  char constexpr path_separator = '\\';
  if(result.back() == path_separator)
    result.resize(result.size() - 1);
  result.append(path.data(), path.size());
  for(auto& c : result)
    if(c == '/')
        c = path_separator;
#else
  char constexpr path_separator = '/';
  if(result.back() == path_separator)
    result.resize(result.size() - 1);
  result.append(path.data(), path.size());
#endif
  return result;
}

/// \todo refactor to HTTP request router
// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<
  class Body
  , class Allocator
  , class SendCallback>
void
handleRequest(
  beast::string_view doc_root,
  beast::http::request<Body, basic_fields<Allocator>>&& req,
  const std::optional<HttpChannel::ResponseToRequestType>& custom_response,
  SendCallback&& sendCallback)
{
  // Returns a bad request response
  auto const bad_request =
  [&req](beast::string_view why)
  {
    DVLOG(99)
      << "HTTPChannel sent `bad request` response";

    HttpChannel::ResponseToRequestType res{
      beast::http::status::bad_request
      , req.version()};

    /// \todo make configurable
    res.set(beast::http::field::server
      , BOOST_BEAST_VERSION_STRING);

    res.set(beast::http::field::content_type
      , "text/html");

    res.keep_alive(req.keep_alive());

    res.body()
      = std::string(why);

    res.prepare_payload();

    return res;
  };

  // Returns a not found response
  auto const not_found =
  [&req](beast::string_view target)
  {
    DVLOG(99)
      << "HTTPChannel sent `not found` response";

    HttpChannel::ResponseToRequestType res{
      beast::http::status::not_found
      , req.version()};

    /// \todo make configurable
    res.set(beast::http::field::server
      , BOOST_BEAST_VERSION_STRING);

    res.set(beast::http::field::content_type
      , "text/html");

    res.keep_alive(req.keep_alive());

    res.body()
      = "The resource '"
        + std::string(target)
        + "' was not found.";

    res.prepare_payload();

    return res;
  };

  // Returns a server error response
  auto const server_error =
  [&req](beast::string_view what)
  {
    DVLOG(99)
      << "HTTPChannel sent `server error` response";

    HttpChannel::ResponseToRequestType res{
      beast::http::status::internal_server_error
      , req.version()};

    /// \todo make configurable
    res.set(beast::http::field::server
      , BOOST_BEAST_VERSION_STRING);

    res.set(beast::http::field::content_type
      , "text/html");

    res.keep_alive(req.keep_alive());

    res.body()
      = "An error occurred: '"
        + std::string(what)
        + "'";

    res.prepare_payload();

    return res;
  };

  if(custom_response) {
    /// \todo support custom_response
    //return sendCallback(custom_response.value());
    NOTIMPLEMENTED();
    return sendCallback(bad_request("Not allowed"));
  }

  // Make sure we can handle the method
  if(req.method() != beast::http::verb::get
     && req.method() != beast::http::verb::head)
  {
    DVLOG(99)
      << "HTTPChannel sent `Unknown HTTP-method` response";
    return sendCallback(bad_request("Unknown HTTP-method"));
  }

  // Request path must be absolute and not contain "..".
  if( req.target().empty() ||
      req.target()[0] != '/' ||
      req.target().find("..") != beast::string_view::npos)
  {
    DVLOG(99)
      << "HTTPChannel sent `Illegal request-target` response";
    return sendCallback(bad_request("Illegal request-target"));
  }

  // Build the path to the requested file
  std::string path = pathConcat(doc_root, req.target());
  if(req.target().back() == '/')
  {
    /// \todo make customizable
    path.append("index.html");
  }

  // Attempt to open the file
  beast::error_code ec;
  beast::http::file_body::value_type fileBody;
  fileBody.open(path.c_str(), beast::file_mode::scan, ec);

  // Handle the case where the file doesn't exist
  if(ec == boost::system::errc::no_such_file_or_directory)
  {
    DVLOG(99)
      << "HTTPChannel unable to find file with path: "
      << path;
    return sendCallback(not_found(req.target()));
  }

  // Handle an unknown error
  if(ec)
  {
    DVLOG(99)
      << "HTTPChannel detected unknown error: "
      << ec.message();
    return sendCallback(server_error(ec.message()));
  }

  // Cache the size since we need it after the move
  auto const size = fileBody.size();

  const static flexnet::MimeType mime_type;

  // Respond to HEAD request
  if(req.method() == beast::http::verb::head)
  {
    DVLOG(99)
      << "HTTPChannel detected `head` request";

    HttpChannel::ResponseEmptyType res{
      beast::http::status::ok, req.version()};

    res.set(beast::http::field::server
      , BOOST_BEAST_VERSION_STRING);

    res.set(beast::http::field::content_type
      , mime_type(path));

    res.content_length(size);

    res.keep_alive(req.keep_alive());

    return sendCallback(RVALUE_CAST(res));
  }

  // Respond to GET request
  DCHECK(req.method() == beast::http::verb::get);
    DVLOG(99)
      << "HTTPChannel detected `get` request";
  HttpChannel::ResponseFileType res{
    std::piecewise_construct,
    std::make_tuple(RVALUE_CAST(fileBody))
    , std::make_tuple(
        beast::http::status::ok
        , req.version())};

  res.set(beast::http::field::server
    , BOOST_BEAST_VERSION_STRING);

  res.set(beast::http::field::content_type
    , mime_type(path));

  res.content_length(size);

  res.keep_alive(req.keep_alive());

  return sendCallback(RVALUE_CAST(res));
}

} // namespace

namespace flexnet {
namespace http {

HttpChannel::HttpChannel(
  StreamType&& stream
  , MessageBufferType&& buffer
  , ECS::SafeRegistry& registry
  , const ECS::Entity entity_id)
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(weak_ptr_factory_.GetWeakPtr()))
  , stream_(RVALUE_CAST(stream))
  , perConnectionStrand_(
      /// \note `get_executor` returns copy
      stream_.get_executor())
  , is_stream_valid_(true)
  , buffer_(RVALUE_CAST(buffer))
  , registry_(REFERENCED(registry))
  , entity_id_(entity_id)
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);

  SET_DEBUG_ATOMIC_FLAG(can_schedule_callbacks_);

  /// \note we assume that configuring `stream_`
  /// is thread-safe here
  {
    // The policy object, which is default constructed, or
    // decay-copied upon construction, is attached to the stream
    // and may be accessed through the function `rate_policy`.
    //
    // Here we set individual rate limits for reading and writing
    stream_.rate_policy()
      /// \todo make configurable
      .read_limit(10000); // bytes per second
    stream_.rate_policy()
      /// \todo make configurable
      .write_limit(850000); // bytes per second
  }
}

HttpChannel::~HttpChannel()
{
  LOG_CALL(DVLOG(99));

  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK_UNOWNED_REF(registry_);

  /// \note do not call `close()` from destructor
  /// i.e. call `close()` manually
  if(is_stream_valid_.load())
  {
    /// \note we assume that reading unused `stream_`
    /// is thread-safe here
    DCHECK(!stream_.socket().is_open());
  }
}

bool HttpChannel::isOpen() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  DCHECK(is_stream_valid_.load());
  return stream_.socket().is_open();
}

void HttpChannel::startReadAsync() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK(!perConnectionStrand_->running_in_this_thread())
    << "use HttpChannel::doRead()";

  /// \note it is not hot code path,
  /// so it is ok to use `base::Promise` here
  DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
  ignore_result(
    postTaskOnConnectionStrand(
      FROM_HERE
      , ::base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &HttpChannel::doRead,
          ::base::Unretained(this)))
  );
}

void HttpChannel::startRead() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  /// \todo Init resources here before first call to `doRead()`

  doRead();
}

void HttpChannel::doRead() NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  DCHECK(is_stream_valid_.load());

  auto& socket
    = beast::get_lowest_layer(stream_).socket();

  if(socket.is_open())
  {
    DVLOG(99)
      << "HTTP read remote_endpoint: "
      << beast::get_lowest_layer(stream_)
          .socket()
          /// \note Transport endpoint must be connected i.e. `is_open()`
          .remote_endpoint();
  }

  // Construct a new parser for each message
  parser_.emplace();

  // Apply a reasonable limit to the allowed size
  // of the body in bytes to prevent abuse.
  parser_->body_limit(kMaxMessageSizeByte);

  // Set the timeout.
  beast::get_lowest_layer(stream_)
    .expires_after(std::chrono::seconds(kExpireTimeoutSec));

  // Read a request
  DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
  beast::http::async_read(
    stream_
    , buffer_
    , parser_->get()
    /// \todo use ::base::BindFrontWrapper
    , boost::asio::bind_executor(
        *perConnectionStrand_
        , ::basis::bindFrontOnceCallback(
            ::base::bindCheckedOnce(
              DEBUG_BIND_CHECKS(
                PTR_CHECKER(this)
              )
              , &HttpChannel::onRead
              , ::base::Unretained(this)
            )
          )
      )
  );
}

void HttpChannel::doEof() NO_EXCEPTION
{
  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  // prohibit callback execution while performing object invalidation.
  UNSET_DEBUG_ATOMIC_FLAG(can_schedule_callbacks_);

  DCHECK(is_stream_valid_.load());

  StreamType::socket_type& socket
    = beast::get_lowest_layer(stream_).socket();

  if(socket.is_open())
  {
    DVLOG(99)
      << "HTTPChannel::do_eof for remote_endpoint: "
      /// \note Transport endpoint must be connected i.e. `is_open()`
      << socket.remote_endpoint();

    // Set the timeout.
    beast::get_lowest_layer(stream_)
      .expires_after(std::chrono::seconds(kCloseTimeoutSec));
  }

  util::closeSocketUnsafe(
    REFERENCED(socket));

  DCHECK_NO_ATOMIC_FLAG(can_schedule_callbacks_);
  registry_.taskRunner()->PostTask(
    FROM_HERE
    , ::base::BindOnce(
        &HttpChannel::markUnused
        , REFERENCED(registry_)
        , entity_id_
      )
  );
}

void HttpChannel::markUnused(
  ECS::SafeRegistry& registry
  , ECS::EntityId entity_id) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON_REGISTRY(&registry);

  // If object was freed,
  // than no need to mark it as unused
  if(!registry->valid(entity_id)){
    return;
  }

  if(!registry->has<ECS::UnusedTag>(entity_id)) {
    registry->emplace<ECS::UnusedTag>(entity_id);
  }
}

void HttpChannel::onFail(
  ErrorCode ec
  , char const* what) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_VALID_PTR(what);

  DCHECK(is_stream_valid_.load());

  // log errors with different log levels
  // (log level based on error code)
  {
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
      DVLOG(99)
        << "HttpChannel::onFail (stream_truncated): "
        << what
        << " : "
        << ec.message();
    }
    else if (ec == ::boost::asio::error::operation_aborted
        || ec == ::boost::beast::websocket::error::closed)
    {
      DVLOG(99)
        << "HttpChannel::onFail (operation_aborted or closed): "
        << what
        << " : "
        << ec.message();
    }
    else if (ec == ::boost::beast::error::timeout)
    {
      DVLOG(99)
        << "HttpChannel::onFail (timeout): "
        << what
        << " : "
        << ec.message();
    } else {
      LOG(WARNING)
        << "HttpChannel::onFail: "
        << what
        << " : "
        << ec.message();
    }
  }

  doEof();
}

void HttpChannel::onRead(
  ErrorCode ec
  , std::size_t bytes_transferred) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  // This means they closed the connection
  if(ec == beast::http::error::end_of_stream) {
    DVLOG(99)
      << "(HttpChannel) remote endpoint closed connection: "
      << ec.message();
    doEof();
    return;
  }

  // Handle the error, if any
  if(ec) {
    onFail(ec, "read");
    return;
  }

  /// \todo create component ECS::HttpReadResult

  DCHECK(is_stream_valid_.load());

  auto& socket
    = beast::get_lowest_layer(stream_).socket();

  // See if it is a WebSocket Upgrade
  if(::boost::beast::websocket::is_upgrade(parser_->get()))
  {
    if(socket.is_open())
    {
      DVLOG(99)
        << "HTTPChannel websocket upgrade for remote_endpoint: "
        /// \note Transport endpoint must be connected i.e. `is_open()`
        << socket.remote_endpoint();
    }

    // Disable the timeout.
    // The websocket::stream uses its own timeout settings.
    beast::get_lowest_layer(stream_).expires_never();

    // create websocket connection
    DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
    registry_.taskRunner()->PostTask(
      FROM_HERE
      , ::base::bindCheckedOnce(
          DEBUG_BIND_CHECKS(
            PTR_CHECKER(this)
          )
          , &HttpChannel::handleWebsocketUpgrade
          , ::base::Unretained(this)
          , ec
          , bytes_transferred
          , RVALUE_CAST(stream_)
          , ::base::Passed(RVALUE_CAST(parser_->release()))
        )
    );

    /// \note `stream_` can be moved to websocket session from http session,
    // so we can't use it here anymore
    is_stream_valid_ = false;

    DVLOG(99)
      << "is_stream_valid_ = false";

    return;
  }

  DCHECK(is_stream_valid_.load());

  /// \todo use send queue https://github.com/OzzieIsaacs/winmerge-qt/blob/master/ext/boost_1_70_0/libs/beast/example/advanced/server-flex/advanced_server_flex.cpp#L616
  // Send the response
  //
  // The following code requires generic
  // lambdas, available in C++14 and later.
  //
  handleRequest(
    /// \todo make configurable
    "/", // doc_root
    parser_->release(),
    std::nullopt, // optional custom response
    [this](auto&& response)
    {
      DCHECK_MEMBER_GUARD(perConnectionStrand_);

      DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

      // `ResponseType` may be `ResponseFileType`, `ResponseToRequestType`, etc.
      using ResponseType
        = typename std::decay<decltype(response)>::type;

      /// \todo remove shared_ptr
      auto sp = std::make_shared<ResponseType>(
        FORWARD(response));

      // Write the response
      // The lifetime of the message `response` has to extend
      // for the duration of the async operation
      // (you can use a shared_ptr to manage it).
      DCHECK_HAS_ATOMIC_FLAG(can_schedule_callbacks_);
      beast::http::async_write(stream_
        , *sp
        , boost::asio::bind_executor(
            *perConnectionStrand_
            , ::basis::bindFrontOnceCallback(
                ::base::bindCheckedOnce(
                  DEBUG_BIND_CHECKS(
                    PTR_CHECKER(this)
                  )
                  , &HttpChannel::processWrittenResponse<ResponseType>
                  , ::base::Unretained(this)
                  // extend lifetime of the message (shared_ptr)
                  , sp
                )
              )
          )
      );
    }
  );
}

void HttpChannel::handleWebsocketUpgrade(
  HttpChannel::ErrorCode ec
  , std::size_t bytes_transferred
  , StreamType&& stream
  , boost::beast::http::request<
      RequestBodyType
    >&& req) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK(registry_.RunsTasksInCurrentSequence());

  ECS::TcpConnection& tcpComponent
    = registry_->get<ECS::TcpConnection>(entity_id_);

  DVLOG(99)
    << "using TcpConnection with id: "
    << tcpComponent.debug_id;

  /// \note it is not ordinary ECS component,
  /// it is stored in entity context (not in ECS registry)
  using WsChannelComponent
    = ::base::Optional<::flexnet::ws::WsChannel>;

  WsChannelComponent* wsChannelCtx
    = &tcpComponent->reset_or_create_var<WsChannelComponent>(
        "Ctx_WsChannelComponent_" + ::base::GenerateGUID() // debug name
        , RVALUE_CAST(stream)
        , REFERENCED(registry_)
        , entity_id_);

  // we moved `stream_` out
  is_stream_valid_.store(false);

  // Check that if the value already existed
  // it was overwritten
  {
    DCHECK(wsChannelCtx->value().entityId() == entity_id_);
  }

  // start websocket session
  ::flexnet::ws::WsChannel& wsChannel
    = const_cast<::flexnet::ws::WsChannel&>(wsChannelCtx->value());

  wsChannel.startAcceptAsync(RVALUE_CAST(req));
}

void HttpChannel::onWrite(
  ErrorCode ec
  , std::size_t bytes_transferred
  , bool closeInResponse) NO_EXCEPTION
{
  LOG_CALL(DVLOG(99));

  DCHECK_MEMBER_GUARD(perConnectionStrand_);

  DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);

  DCHECK(is_stream_valid_.load());

  boost::ignore_unused(bytes_transferred);

  // Handle the error, if any
  if(ec) {
    onFail(ec, "write");
    return;
  }

  if(closeInResponse) {
    // This means we should close the connection, usually because
    // the response indicated the "Connection: close" semantic.
    doEof();
    return;
  }

  // Read another request
  doRead();
}

} // namespace http
} // namespace flexnet
