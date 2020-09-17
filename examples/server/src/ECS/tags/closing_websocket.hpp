#pragma once

#include <basis/ECS/ecs.hpp>

namespace ECS {

// do not schedule `async_close` twice
CREATE_ECS_TAG(ClosingWebsocket)

} // namespace ECS
