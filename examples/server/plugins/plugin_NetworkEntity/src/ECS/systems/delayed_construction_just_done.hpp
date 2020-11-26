#include "basis/ECS/safe_registry.hpp"

#include <chrono>

namespace ECS {

// Handle `ECS::DelayedConstructionJustDone`
/// \note Removes `DelayedConstructionJustDone` component from any entity.
//
// HOT-CODE PATH
//
// Use plain callbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateDelayedConstructionJustDone(
  ECS::SafeRegistry& registry);

} // namespace ECS
