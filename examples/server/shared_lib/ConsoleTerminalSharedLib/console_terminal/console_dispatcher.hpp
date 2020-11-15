#pragma once

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <basis/strong_types/strong_alias.hpp>

namespace backend {

// UNIQUE type to store in sequence-local-context
STRONGLY_TYPED(entt::dispatcher, ConsoleTerminalEventDispatcher);

} // namespace backend
