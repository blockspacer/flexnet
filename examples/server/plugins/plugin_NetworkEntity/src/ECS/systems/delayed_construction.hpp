#include "basis/ECS/network_registry.hpp"

#include <chrono>

namespace ECS {

// Handle `ECS::DelayedConstruction`
/// \note Removes `DelayedConstruction` component from any entity
/// and adds `DelayedConstructionJustDone` component.
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateDelayedConstruction(
  ECS::NetworkRegistry& net_registry);

} // namespace ECS
