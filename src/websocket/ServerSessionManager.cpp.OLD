#include "net/ws/server/ServerSessionManager.hpp" // IWYU pragma: associated
#include "net/ws/SessionGUID.hpp"
#include "net/ws/server/ServerSession.hpp"

namespace beast = boost::beast;               // from <boost/beast.hpp>
//namespace http = beast::http;                 // from <boost/beast/http.hpp>
namespace websocket = beast::websocket;       // from <boost/beast/websocket.hpp>
//namespace net = boost::asio;                  // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;             // from <boost/asio/ip/tcp.hpp>
using error_code = boost::system::error_code; // from <boost/system/error_code.hpp>

namespace flexnet {
namespace ws {

/**
 * @brief removes session from list of valid sessions
 *
 * @param id id of session to be removed
 */
void ServerSessionManager::unregisterSession(const ws::SessionGUID& id)
{
  LOG(WARNING) << "(ws server) unregisterSession for id = " << static_cast<std::string>(id);
  const ws::SessionGUID idCopy = id; // unknown lifetime, use idCopy
  std::shared_ptr<ws::ServerSession> sess = getSessById(idCopy);
  DCHECK(sess);
  DCHECK(!sess->isOpen()); /// \note close session before forgetting about it

  if (!removeSessById(idCopy)) {
    LOG(WARNING) << "WsServer::unregisterSession: trying to unregister non-existing session "
                  << static_cast<std::string>(idCopy);
    // NOTE: continue cleanup with saved shared_ptr
  }

  LOG(WARNING) << "WsServer: unregistered " << static_cast<std::string>(idCopy);
}

ServerSessionManager::ServerSessionManager()
{}

} // namespace ws
} // namespace flexnet
