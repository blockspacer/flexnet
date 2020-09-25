#pragma once

#include <string>
#include <vector>
#include <Corrade/PluginManager/AbstractPlugin.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/PluginManager.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/PluginManager/PluginMetadata.h>
#include <Corrade/Utility/Arguments.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Debug.h>
#include <Corrade/Utility/DebugStl.h>
#include <Corrade/Utility/Directory.h>
#include <Corrade/Containers/Array.h>

#include <entt/entt.hpp>
#include <entt/entity/registry.hpp>
#include <entt/entity/helper.hpp>
#include <entt/entity/group.hpp>

#include <base/logging.h>
#include <base/cpu.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/trace_event/trace_event.h>

#include <basis/promise/post_promise.h>

#define REGISTER_PLUGIN(name, className, interface) \
  CORRADE_PLUGIN_REGISTER(name, className, interface)

namespace plugin {

typedef ::Corrade::PluginManager::AbstractManager AbstractManager;
typedef ::Corrade::PluginManager::AbstractPlugin AbstractPlugin;

class PluginInterface
  : public ::plugin::AbstractPlugin
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  explicit PluginInterface(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : AbstractPlugin{manager, pluginName}
  {
  }

  static std::string pluginInterface() {
    // plugin interface version checks to avoid unexpected behavior
    return "plugin.PluginInterface/1.0";
  }

  static std::vector<std::string> pluginSearchPaths() {
    return {""};
  }

  virtual std::string author() const = 0;

  virtual std::string title() const = 0;

  virtual std::string description() const = 0;

  virtual VoidPromise load() = 0;

  virtual VoidPromise unload() = 0;

protected:
  DISALLOW_COPY_AND_ASSIGN(PluginInterface);
};

} // namespace plugin
