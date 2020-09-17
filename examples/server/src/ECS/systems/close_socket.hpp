#include "basis/ECS/asio_registry.hpp"

#include <flexnet/websocket/listener.hpp>

#include <chrono>

namespace ECS {

/// \FIXME remove `ECS::CloseSocket`
// Handle `ECS::CloseSocket`
//
// HOT-CODE PATH
//
// Use plain collbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateClosingSockets(
  ECS::AsioRegistry& asio_registry);

} // namespace ECS
