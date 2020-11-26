#include "basis/ECS/safe_registry.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

#include <chrono>

namespace ECS {

// Handle `Listener::AcceptConnectionResult`
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateNewConnections(
  ECS::SafeRegistry& registry);

} // namespace ECS
