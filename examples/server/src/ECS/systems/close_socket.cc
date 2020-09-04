#include "ECS/systems/close_socket.hpp" // IWYU pragma: associated

#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>
#include <flexnet/ECS/components/close_socket.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/guid.h>

namespace ECS {

void handleClosingSocket(
  ECS::AsioRegistry& asio_registry
  , const ECS::Entity& entity_id
  , ECS::CloseSocket& component)
{
  using namespace ::flexnet::ws;

  DCHECK(asio_registry.running_in_this_thread());

  ECS::TcpConnection& tcpComponent
    = asio_registry->get<ECS::TcpConnection>(entity_id);

  DVLOG(99)
    << "closing socket of connection with id: "
    << tcpComponent.debug_id;

  DCHECK(tcpComponent->try_ctx_var<flexnet::ws::Listener::StrandComponent>());
  Listener::StrandComponent& strandComponent
    = tcpComponent->ctx_var<flexnet::ws::Listener::StrandComponent>();

  base::OnceClosure doMarkUnused
    = base::BindOnce(
        []
        (ECS::AsioRegistry& asio_registry
         , ECS::Entity entity_id)
        {
          DCHECK(asio_registry.running_in_this_thread());

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
        (ECS::CloseSocket::SocketType* socket
         , ECS::AsioRegistry& asio_registry
         , base::OnceClosure&& task)
        {
          if(!socket->is_open())
          {
            DVLOG(99)
              << "skipping shutdown of already closed"
                 " asio socket...";
          }

          DVLOG(99)
            << "shutdown of asio socket...";

          boost::beast::error_code ec;

          socket->shutdown(
            boost::asio::ip::tcp::socket::shutdown_send, ec);

          if (ec) {
            LOG(WARNING)
              << "error during shutdown of asio socket: "
              << ec.message();
          }

          // Schedule change on registry thread
          ::boost::asio::post(
            asio_registry.strand()
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
        , COPIED() component.socket
        , REFERENCED(asio_registry)
        , base::rvalue_cast(doMarkUnused)
      );

  // Schedule shutdown on asio thread
  ::boost::asio::post(
    strandComponent
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

  DCHECK(asio_registry.running_in_this_thread());

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
