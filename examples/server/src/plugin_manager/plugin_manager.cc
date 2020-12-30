#include "plugin_manager.hpp" // IWYU pragma: associated

#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/Utility/Configuration.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Directory.h>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/rvalue_cast.h>

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <utility>

#ifdef CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT
#error \
  "no CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT" \
  " for that platform"
#endif

namespace plugin {

// extern
const char kDefaultPluginsDirName[]
  = "plugins";

// extern
const char kPluginsConfigFileName[]
  = "plugins.conf";

// extern
const char kAllPluginsConfigCategory[]
  = "plugins";

// extern
const char kIndividualPluginConfigCategory[]
  = "plugin";

using AbstractPlugin
  = ::Corrade::PluginManager::AbstractPlugin;

using AbstractManager
  = ::Corrade::PluginManager::AbstractManager;

using Configuration
  = ::Corrade::Utility::Configuration;

using ConfigurationGroup
  = ::Corrade::Utility::ConfigurationGroup;

using LoadState
  = ::Corrade::PluginManager::LoadState;

bool parsePluginsConfig(
  Configuration& conf
  , std::vector<ConfigurationGroup*>& plugin_groups
  , bool& is_plugin_filtering_enabled)
{
  if(!conf.hasGroups()) {
    VLOG(9)
      << "unable to find"
         " any configuration groups in file: "
      << conf.filename();
    return false;
  }

  if(!conf.hasGroup(plugin::kAllPluginsConfigCategory)) {
    VLOG(9)
      << "unable to find"
         " configuration group: "
      << plugin::kAllPluginsConfigCategory
      << " in file: "
      << conf.filename();
    return false;
  }

  is_plugin_filtering_enabled = true;

  // configurations for all plugins
  ConfigurationGroup* configurationGroup
    = conf.group(plugin::kAllPluginsConfigCategory);
  DCHECK_PTR(configurationGroup);

  if(configurationGroup) {
    // configurations for individual plugins
    plugin_groups = configurationGroup->groups(
      plugin::kIndividualPluginConfigCategory);
  } else {
    NOTREACHED();
  }

  return true;
}

std::map<std::string, PluginMetadata> loadPluginsMetadata(
  std::vector<
      ::Corrade::Utility::ConfigurationGroup*
    >& plugin_groups
  , std::vector<std::string>& dynamic_plugins
  , std::vector<std::string>& static_plugins)
{
  std::map<std::string, PluginMetadata> filtered_plugins;

  for(const ConfigurationGroup* plugin_group
      : plugin_groups)
  {
    DCHECK_PTR(plugin_group);

    std::vector<std::string> dependsOn;

    if(!plugin_group->hasValue("title"))
    {
      LOG(WARNING)
          << "invalid plugin configuration: "
          << "title not provided";
      DCHECK(false);
      continue;
    }

    const std::string pluginTitle
      = plugin_group->value("title");

    if(plugin_group->hasValue("depends"))
    {
      DVLOG(99)
        << "found sequential plugin group: "
        << pluginTitle;

      /// \note It is allowed to have more than one value
      /// with the same key.
      /// You can access all values for given key name
      /// using values().
      //
      // EXAMPLE
      //
      // depends=bread
      // depends=milk
      // depends=apples
      // depends=onions
      for(const std::string& dep
          : plugin_group->values("depends"))
      {
        dependsOn.push_back(dep);
      }
    } else {
      DVLOG(99)
        << "found parrallel plugin group: "
        << pluginTitle;
    }

    // If plugin name found in config,
    // than need to load plugin.
    auto find_result_dynamic
      = std::find(dynamic_plugins.begin()
                  , dynamic_plugins.end()
                  , pluginTitle);
    auto find_result_static
      = std::find(static_plugins.begin()
                  , static_plugins.end()
                  , pluginTitle);
    if(find_result_dynamic == dynamic_plugins.end()
       && find_result_static == static_plugins.end())
    {
      // can not find dynamic or static plugin
      // with desired name
      CHECK(false)
          << "plugin not found: "
          << pluginTitle;
    } else {
      filtered_plugins[pluginTitle]
        = PluginMetadata{
            pluginTitle
            , RVALUE_CAST(dependsOn)
            , std::vector<std::string>{} // requiredBy
          };
    }
  }

  // populate `requiredBy`
  for(std::map<std::string, PluginMetadata>::const_iterator it =
        filtered_plugins.begin()
      ; it != filtered_plugins.end()
      ; ++it)
  {
    for(const std::string& dep
        : (*it).second.dependsOn)
    {
      filtered_plugins[dep]
        .requiredBy
        .push_back((*it).second.pluginName);
    }
  }

  return filtered_plugins;
}

} // namespace plugin
