#pragma once

#include <Corrade/Containers/Pointer.h>
#include <Corrade/PluginManager/AbstractManager.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/Utility/Configuration.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Directory.h>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/macros.h>
#include <base/sequence_checker.h>

#include <basis/promise/post_promise.h>

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <utility>
#include <stddef.h>
#include <string>
#include <vector>

namespace plugin {

extern
const char kDefaultPluginsDirName[];

extern
const char kPluginsConfigFileName[];

extern
const char kAllPluginsConfigCategory[];

extern
const char kIndividualPluginConfigCategory[];

/// \note if configuration file does not have `[plugins]`
/// section, then `is_plugin_filtering_enabled`
/// will keep initial value (usually false).
/**
 * EXAMPLE CONFIG FILE:
 *
# configuration file format https://doc.magnum.graphics/corrade/classCorrade_1_1Utility_1_1Configuration.html

# you can remove plugins section to load all plugins from plugins directory
[plugins]

[plugins/plugin]
title=ExtraLogging
type=static

#[plugins/plugin]
#title=ExtraCmd
**/
[[nodiscard]] /* do not ignore return value */
bool parsePluginsConfig(
  ::Corrade::Utility::Configuration& conf
  , std::vector<
      ::Corrade::Utility::ConfigurationGroup*
    >& plugin_groups
  , bool& is_plugin_filtering_enabled);

/// \todo refactor to arbitrary `std::vector<std::string>` intersection
// returns all strings that can be found in `ConfigurationGroup`
std::vector<std::string> filterPluginsByConfig(
  std::vector<
      ::Corrade::Utility::ConfigurationGroup*
    >& plugin_groups
  , std::vector<std::string>& all_plugins);

template<
  typename PluginType
>
class PluginManager {
public:
  using PluginPtr
    = Corrade::Containers::Pointer<
        PluginType
      >;

  using VoidPromise
    = base::Promise<void, base::NoReject>;

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

public:
  PluginManager()
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~PluginManager() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  /// \todo refactor long method
  VoidPromise startup(
    // dir must contain plugin files (usually `.so` files)
    const base::FilePath& _pathToDirWithPlugins
    // path to `plugins.conf`
    , const base::FilePath& _pathToPluginsConfFile
    // used to force loading of some plugins
    , const std::vector<base::FilePath>& _pathsToExtraPluginFiles)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    TRACE_EVENT0("toplevel", "PluginManager::startup()")

    VLOG(9) << "(PluginManager) startup";

    using namespace ::Corrade::Utility::Directory;

    std::vector<VoidPromise> allPromises;

    const std::string executable_path
      = path(
          executableLocation());
    CHECK(!executable_path.empty())
      << "invalid executable path";

    const base::FilePath pathToDirWithPlugins
      = _pathToDirWithPlugins.empty()
        // default value
          ? base::FilePath{executable_path}
          : _pathToDirWithPlugins;
    CHECK(!pathToDirWithPlugins.empty())
      << "invalid path to directory with plugins";

    const std::string pluginsConfFile
      = join(
          std::initializer_list<std::string>{
            pathToDirWithPlugins.value()
            , kPluginsConfigFileName}
          );

    const base::FilePath pathToPluginsConfFile
      = _pathToPluginsConfFile.empty()
        // default value
        ? base::FilePath{pluginsConfFile}
        : _pathToPluginsConfFile;
    CHECK(!pathToPluginsConfFile.empty())
      << "invalid path to plugins configuration file";

    VLOG(9)
      << "using plugins configuration file: "
      << pathToPluginsConfFile;

    Configuration conf{
      pathToPluginsConfFile.value()};

    std::vector<ConfigurationGroup*> plugin_groups;

    // filter plugins based on config
    bool is_plugin_filtering_enabled = false;

    // parse plugins configuration file
    {
      const bool parseOk = parsePluginsConfig(REFERENCED(conf)
        , REFERENCED(plugin_groups)
        , REFERENCED(is_plugin_filtering_enabled)
      );

      if(!parseOk) {
        LOG(WARNING)
          << "unable to parse plugins configuration file: "
          << pathToPluginsConfFile;
      }
    }

    DCHECK(!manager_);
    manager_
      = std::make_unique<
      ::Corrade::PluginManager::Manager<PluginType>
      >();

    /**
     * Set the plugin manager directory to load plugins
     *
     * Keeps loaded plugins untouched, removes unloaded plugins which are
     * not existing anymore and adds newly found plugins. The directory is
     * expected to be in UTF-8.
     * @partialsupport Not available on platforms without
     *      @ref CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT "dynamic plugin support".
     */
    manager_->setPluginDirectory(
      pathToDirWithPlugins.value());

    VLOG(9)
      << "Using plugin directory: "
      << manager_->pluginDirectory();

    std::vector<std::string> all_plugins
      = manager_->pluginList();

    for(const std::string& pluginName: all_plugins) {
      DVLOG(99)
        << "found plugin with name: "
        << pluginName;
    }

    std::vector<std::string> filtered_plugins;

    // parse list of plugin sections from plugins configuration file
    if(is_plugin_filtering_enabled)
    {
      filtered_plugins
        = filterPluginsByConfig(plugin_groups
            , all_plugins
          );
    }

    // append path to plugins that
    // must be loaded independently of configuration file
    for(const base::FilePath& pluginPath
        : _pathsToExtraPluginFiles)
    {
      VLOG(9)
          << "added plugin: "
          << pluginPath;
      if(pluginPath.empty() || !base::PathExists(pluginPath)) {
        LOG(ERROR)
          << "invalid path to plugin file: "
          << pluginPath;
        CHECK(false)
          << "path does not exist: "
          << pluginPath;
      }
      filtered_plugins.push_back(
        /*
        * @note If passing a file path, the implementation expects forward
        *      slashes as directory separators.
        * Use @ref Utility::Directory::fromNativeSeparators()
        *      to convert from platform-specific format.
        */
        fromNativeSeparators(
          pluginPath.value()));
    }

    DCHECK(loaded_plugins_.empty())
      << "Plugin manager must load plugins once.";

    for(std::vector<std::string>::const_iterator it =
          filtered_plugins.begin()
        ; it != filtered_plugins.end(); ++it)
    {
      const std::string& pluginNameOrPath = *it;

      // The implementation expects forward slashes as directory separators.
      DCHECK(fromNativeSeparators(
               pluginNameOrPath) == pluginNameOrPath);

      VLOG(9)
          << "plugin enabled: "
          << pluginNameOrPath;

      /**
       * @brief Load a plugin
       *
       * Returns @ref LoadState::Loaded if the plugin is already loaded or if
       * loading succeeded. For static plugins returns always
       * @ref LoadState::Static. On failure returns @ref LoadState::NotFound,
       * @ref LoadState::WrongPluginVersion,
       * @ref LoadState::WrongInterfaceVersion,
       * @ref LoadState::UnresolvedDependency or @ref LoadState::LoadFailed.
       *
       * If the plugin has any dependencies, they are recursively processed
       * before loading given plugin.
       *
       * If @p plugin is a plugin file path (i.e., ending with a
       * platform-specific extension such as `.so` or `.dll`), it's loaded
       * from given location and, if the loading succeeds, its basename
       * (without extension) is exposed as an available plugin name. It's
       * expected that a plugin with the same name is not already loaded. The
       * plugin will reside in the plugin list as long as it's loaded or,
       * after calling @ref unload() on it, until next call to
       * @ref setPluginDirectory() or @ref reloadPluginDirectory().
       *
       * @note If passing a file path, the implementation expects forward
       *      slashes as directory separators. Use @ref Utility::Directory::fromNativeSeparators()
       *      to convert from platform-specific format.
       *
       * @see @ref unload(), @ref loadState(), @ref Manager::instantiate(),
       *      @ref Manager::loadAndInstantiate()
       * @partialsupport On platforms without
       *      @ref CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT "dynamic plugin support"
       *      returns always either @ref LoadState::Static or
       *      @ref LoadState::NotFound.
      **/
      DCHECK(manager_);

      const LoadState loadState
        = manager_->load(pluginNameOrPath);

      if(loadState & LoadState::Loaded) {
        DVLOG(99)
          << "loaded state for plugin: "
          << pluginNameOrPath;
      }

      if(loadState & LoadState::Static) {
        DVLOG(99)
          << "found static load state for plugin: "
          << pluginNameOrPath;
      } else {
        const bool is_loaded
          = static_cast<bool>(
              loadState
              & (LoadState::Loaded
                 | LoadState::
                   Static)
              );
        if(!is_loaded) {
          LOG(ERROR)
            << "The requested plugin "
            << pluginNameOrPath
            << " cannot be loaded.";
          DCHECK(false);
          continue;
        }
      }

      /**
      @brief Extract filename (without path) from filename

      If the filename doesn't contain any slash, returns whole string, otherwise
      returns everything after last slash.
      @attention The implementation expects forward slashes as directory separators.
          Use @ref fromNativeSeparators() to convert from a platform-specific format.
      @see @ref path(), @ref splitExtension()
      */
      const std::string pluginNameOrFilenameWithExt
        = filename(pluginNameOrPath);

      DCHECK(base::FilePath{pluginNameOrPath}
               .BaseName().value()
             == pluginNameOrFilenameWithExt);

      /**
      @brief Split basename and extension
      @m_since{2019,10}

      Returns a pair `(root, ext)` where @cpp root + ext == path @ce, and ext is
      empty or begins with a period and contains at most one period. Leading periods
      on the filename are ignored, @cpp splitExtension("/home/.bashrc") @ce returns
      @cpp ("/home/.bashrc", "") @ce. Behavior equivalent to Python's
      @cb{.py} os.path.splitext() @ce.
      @attention The implementation expects forward slashes as directory separators.
          Use @ref fromNativeSeparators() to convert from a platform-specific format.
      @see @ref path(), @ref filename(), @ref String::partition()
      */
      const std::string pluginName
        = splitExtension(
            pluginNameOrFilenameWithExt).first;

      DCHECK(base::FilePath{pluginNameOrFilenameWithExt}
               .BaseName().RemoveExtension().value()
             == pluginName);

      DCHECK(manager_ && static_cast<bool>(
               manager_->loadState(pluginName)
               & (LoadState::
                    Loaded
                  | LoadState::
                    Static)
               ));

      /// Returns new instance of given plugin.
      /// \note The plugin must be already
      /// successfully loaded by this manager.
      /// \note The returned value is never |nullptr|
      DCHECK(manager_);
      PluginPtr plugin
      /// \note must be plugin name, not path to file
        = manager_->instantiate(pluginName);
      if(!plugin) {
        LOG(ERROR)
          << "The requested plugin "
          << pluginNameOrPath
          << " cannot be instantiated.";
        DCHECK(false);
        continue;
      }

      VLOG(9)
        << "=== loading plugin ==";
      VLOG(9)
          << "plugin title:       "
          << plugin->title();
      VLOG(9)
          << "plugin description: "
          << plugin->description().substr(0, 100)
          << "...";

      VoidPromise loadPromise
        = plugin->load();
      allPromises.push_back(loadPromise);

      loaded_plugins_.push_back(std::move(plugin));
      VLOG(9)
        << "=== plugin loaded ==";
    }

    DCHECK(!is_initialized_)
      << "Plugin manager must be initialized once."
      << "You can unload or reload plugins at runtime.";
    is_initialized_ = true;

    return base::Promises::All(FROM_HERE, allPromises);
  }

  // calls `unload()` for each loaded plugin
  VoidPromise shutdown()
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    TRACE_EVENT0("toplevel", "PluginManager::shutdown()");

    VLOG(9) << "(PluginManager) shutdown";

    std::vector<VoidPromise> allPromises;

    /// \note destructor of ::Corrade::PluginManager::Manager
    /// also unloads all plugins
    for(PluginPtr& loaded_plugin : loaded_plugins_) {
      DCHECK(loaded_plugin);
      VoidPromise unloadPromise
        = loaded_plugin->unload();
      allPromises.push_back(unloadPromise);
    }

    return base::Promises::All(FROM_HERE, allPromises);
  }

  [[nodiscard]] /* do not ignore return value */
  size_t countLoadedPlugins()
  const noexcept
  {
    return
      loaded_plugins_.size();
  }

private:
  bool is_initialized_ = false;

  std::unique_ptr<
    ::Corrade::PluginManager::Manager<PluginType>
    > manager_;

  std::vector<PluginPtr> loaded_plugins_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(PluginManager);
};

} // namespace plugin
