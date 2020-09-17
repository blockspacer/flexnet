#include "ECS/systems/close_socket.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/guid.h>

namespace ECS {

using Listener
  = flexnet::ws::Listener;

void handleClosingSocket(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , ECS::CloseSocket& component)
{
  using namespace ::flexnet::ws;

  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  DCHECK(asio_registry->valid(entity_id));

  ECS::TcpConnection& tcpComponent
    = asio_registry->get<ECS::TcpConnection>(entity_id);

  DVLOG(99)
    << "closing socket of connection with id: "
    << tcpComponent.debug_id;

  // `socket` can be manipulated
  // on user-provided asio `strand`
  // (with fallback to default one).
  /// \note Usually we close socket
  /// when error occured or socket no more used,
  /// so we can assume that is safe to use
  /// any existing strand (or create new strand)
  /// when `component.strand` is `nullptr`.
  ECS::CloseSocket::StrandType* strandComponent
    = component.strandPtr;

  // default strand used as fallback if `component.strand` is `nullptr`
  DCHECK(tcpComponent->try_ctx_var<Listener::StrandComponent>());
  Listener::StrandComponent& fallbackStrandComponent
      = tcpComponent->ctx_var<Listener::StrandComponent>();

  // Fallback to some valid strand
  // if custom strand not provided.
  if(strandComponent == nullptr)
  {
    strandComponent = &(fallbackStrandComponent.value());
  }

  base::OnceClosure doMarkUnused
    = base::BindOnce(
        []
        (ECS::AsioRegistry& asio_registry
         , ECS::Entity entity_id)
        {
          DCHECK(asio_registry.running_in_this_thread());

          // Before marking entity as unused we closed stream,
          // but `per-connection-class`-es may also mark entity as unused
          // or free its memory (because of errors related to closed stream),
          // so need to ensure that entity is valid
          // (because we use asyncronous task system).
          if(!asio_registry->valid(entity_id))
          {
            LOG(WARNING)
              << "unable to mark as unused entity with id = "
              << entity_id;
            return;
          }

          ECS::TcpConnection& tcpComponent
            = asio_registry->get<ECS::TcpConnection>(entity_id);

          DVLOG(99)
            << "marking as unused connection with id: "
            << tcpComponent.debug_id;

          // it is safe to destroy entity now
          if(!asio_registry->has<ECS::UnusedTag>(entity_id)) {
            asio_registry->emplace<ECS::UnusedTag>(entity_id);
          }
        }
        , REFERENCED(asio_registry)
        , COPIED() entity_id
      );

  base::OnceClosure doSocketClose
    = base::BindOnce(
        []
        (ECS::CloseSocket::SocketType* socketPtr
         , ECS::AsioRegistry& asio_registry
         , base::OnceClosure&& task)
        {
          if(socketPtr && socketPtr->is_open())
          {
            DVLOG(99)
              << "shutdown of asio socket...";

            boost::beast::error_code ec;

            // `shutdown_both` disables sends and receives
            // on the socket.
            /// \note Call before `socket.close()` to ensure
            /// that any pending operations on the socket
            /// are properly cancelled and any buffers are flushed
            socketPtr->shutdown(
              boost::asio::ip::tcp::socket::shutdown_both, ec);

            if (ec) {
              LOG(WARNING)
                << "error during shutdown of asio socket: "
                << ec.message();
            }

            // Close the socket.
            // Any asynchronous send, receive
            // or connect operations will be cancelled
            // immediately, and will complete with the
            // boost::asio::error::operation_aborted error.
            /// \note even if the function indicates an error,
            /// the underlying descriptor is closed.
            /// \note For portable behaviour with respect to graceful closure of a
            /// connected socket, call shutdown() before closing the socket.
            socketPtr->close(ec);

            if (ec) {
              LOG(WARNING)
                << "error during close of asio socket: "
                << ec.message();
            }
          } else {
            DVLOG(99)
              << "unable to shutdown already closed asio socket...";
          }

          // Schedule change on registry thread
          ::boost::asio::post(
            asio_registry.asioStrand()
            /// \todo use base::BindFrontWrapper
            , ::boost::beast::bind_front_handler([
              ](
                base::OnceClosure&& task
              ){
                DCHECK(task);
                base::rvalue_cast(task).Run();
              }
              , base::rvalue_cast(task)
            )
          );
        }
        , COPIED() component.socketPtr // copy only pointer to socket
        , REFERENCED(asio_registry)
        , base::rvalue_cast(doMarkUnused)
      );

  DCHECK(strandComponent);
  // Schedule shutdown on asio thread
  ::boost::asio::post(
    *strandComponent
    /// \todo use base::BindFrontWrapper
    , ::boost::beast::bind_front_handler([
      ](
        base::OnceClosure&& task
      ){
        DCHECK(task);
        base::rvalue_cast(task).Run();
      }
      , base::rvalue_cast(doSocketClose)
    )
  );
}

void updateClosingSockets(
  ECS::AsioRegistry& asio_registry)
{
  using namespace ::flexnet::ws;

  using view_component
    = ECS::CloseSocket;

  DCHECK_RUN_ON_STRAND(&asio_registry.strand, ECS::AsioRegistry::ExecutorType);

  // Avoid extra allocations
  // with memory pool in ECS style using |ECS::UnusedTag|
  // (objects that are no more in use can return into pool)
  /// \note do not forget to free some memory in pool periodically
  auto registry_group
    = asio_registry->view<view_component>(
        entt::exclude<
          // entity in destruction
          ECS::NeedToDestroyTag
          // entity is unused
          , ECS::UnusedTag
        >
      );

  registry_group
    .each(
      [&asio_registry]
      (const auto& entity
       , const auto& component)
    {
      DCHECK(asio_registry->valid(entity));

      handleClosingSocket(
        asio_registry
        , entity
        , asio_registry->get<view_component>(entity));

      // do not process twice
      asio_registry->remove<view_component>(entity);
    });
}

} // namespace ECS
