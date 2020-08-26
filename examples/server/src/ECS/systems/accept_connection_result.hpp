#include "basis/ECS/asio_registry.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

#include <chrono>

namespace ECS {

void updateNewConnections(
  ECS::AsioRegistry& asio_registry);

} // namespace ECS
