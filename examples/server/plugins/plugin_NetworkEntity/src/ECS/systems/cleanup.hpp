#include "basis/ECS/asio_registry.hpp"

#include <chrono>

namespace ECS {

// Destroys entities marked with `ECS::NeedToDestroyTag`
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateCleanupSystem(
  ECS::AsioRegistry& asio_registry);

} // namespace ECS