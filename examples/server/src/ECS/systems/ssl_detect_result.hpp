#include "basis/ECS/asio_registry.hpp"

#include <flexnet/http/detect_channel.hpp>

#include <chrono>

namespace ECS {

// Handle `DetectChannel::SSLDetectResult`
//
// HOT-CODE PATH
//
// Use plain collbacks (do not use `base::Promise` etc.)
// and avoid heap memory allocations
// because performance is critical here.
void updateSSLDetection(
  ECS::AsioRegistry& asio_registry);

} // namespace ECS
