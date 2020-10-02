#include "basis/ECS/network_registry.hpp"

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
  ECS::NetworkRegistry& net_registry);

} // namespace ECS
