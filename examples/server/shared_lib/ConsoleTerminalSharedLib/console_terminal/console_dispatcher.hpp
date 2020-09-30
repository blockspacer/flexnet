#pragma once

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <basis/strong_alias.hpp>

namespace backend {

// UNIQUE type to store in sequence-local-context
using ConsoleTerminalEventDispatcher
  = util::StrongAlias<
      class ConsoleTerminalEventDispatcherTag
      , entt::dispatcher
    >;

} // namespace backend
