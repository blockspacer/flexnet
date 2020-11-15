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
#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>
#include <base/metrics/histogram_functions.h>
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_buffer.h>
#include <base/trace_event/trace_log.h>

#include <basis/promise/post_promise.h>
#include <basis/bind/bind_checked.hpp>
#include <basis/bind/ptr_checker.hpp>

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

struct PluginMetadata
{
  /// \todo support plugin aliases,
  /// not only one plugin name
  std::string pluginName;

  std::vector<std::string> dependsOn;

  std::vector<std::string> requiredBy;
};

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

// Populates plugin metadata based on `title` in each `ConfigurationGroup`.
std::map<std::string, PluginMetadata> loadPluginsMetadata(
  std::vector<
      ::Corrade::Utility::ConfigurationGroup*
    >& plugin_groups
  , std::vector<std::string>& dynamic_plugins
  , std::vector<std::string>& static_plugins);

template<
  typename PluginType
>
class PluginManager {
public:
  using ConfigurationGroup
    = ::Corrade::Utility::ConfigurationGroup;

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

  using LoadState
    = ::Corrade::PluginManager::LoadState;

  struct PluginWithMetadata
  {
    PluginPtr plugin;

    PluginMetadata metadata;
  };

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
      /// \todo support per-plugin config,
      /// not only global config
      const bool parseOk = parsePluginsConfig(
        REFERENCED(conf)
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

    // Static plugins must be available
    // before `setPluginDirectory`.
    // Must NOT intersect with list of dynamic plugins.
    std::vector<std::string> static_plugins
      = manager_->pluginList();

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

    // Dynamic plugins must be available
    // after `setPluginDirectory`.
    // Also contains list of static plugins.
    std::vector<std::string> plugins_to_filter
      = manager_->pluginList();

    std::vector<std::string> dynamic_plugins;

    for(const std::string& pluginName: plugins_to_filter) {
      DVLOG(99)
        << "found plugin with name: "
        << pluginName;

      auto find_result
        = std::find(static_plugins.begin()
                    , static_plugins.end()
                    , pluginName);
      if(find_result == static_plugins.end())
      {
        DVLOG(99)
            << "plugin is dynamic: "
            << pluginName;

        dynamic_plugins.push_back(pluginName);
      } else {
        DVLOG(99)
            << "plugin is static: "
            << pluginName;
      }
    }

    std::map<std::string, PluginMetadata> filtered_plugins;

    // parse list of plugin sections
    // from plugins configuration file
    if(is_plugin_filtering_enabled)
    {
      filtered_plugins
        // some checks, like checks for
        // `.so`/`.dll`/`.conf` file existance
        = loadPluginsMetadata(plugin_groups
            , dynamic_plugins
            , static_plugins
          );
    }

    for(const std::string& pluginName: static_plugins)
    {
      DVLOG(99)
        << "found static plugin with name: "
        << pluginName;
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
      filtered_plugins[pluginPath.value()]
        = PluginMetadata{
            /*
            * @note If passing a file path, the implementation expects forward
            *      slashes as directory separators.
            * Use @ref Utility::Directory::fromNativeSeparators()
            *      to convert from platform-specific format.
            */
            /// \todo support passing plugin name, aliases
            /// and configuration from command-line
            fromNativeSeparators(
              /// \FIXME it is plugin path, not plugin name
              pluginPath.value()) // plugin name
            , std::vector<std::string>{} // dependsOn
            , std::vector<std::string>{} // requiredBy
          };
    }

    DCHECK(loaded_plugins_.empty())
      << "Plugin manager must load plugins once.";

    std::vector<VoidPromise> loadPromiseParallel;

    // init `loadResolvers_` and `unloadResolvers_`
    for(std::map<std::string, PluginMetadata>::const_iterator it =
          filtered_plugins.begin()
        ; it != filtered_plugins.end()
        ; ++it)
    {
      const std::string& nameOrPath = (*it).second.pluginName;

      // required for `load()`
      {
        CHECK(loadResolvers_.find(nameOrPath) == loadResolvers_.end())
          << "unable to load twice for plugin: "
          << nameOrPath;

        loadResolvers_[nameOrPath]
          = std::make_unique<
              base::ManualPromiseResolver<void, base::NoReject>
            >(FROM_HERE);

        CHECK(loadPromises_.find(nameOrPath) == loadPromises_.end())
          << "unable to promise load twice for plugin: "
          << nameOrPath;

        loadPromises_[nameOrPath]
          = loadResolvers_[nameOrPath]->promise();
      }

      // required for `unload()`
      {
        CHECK(unloadResolvers_.find(nameOrPath) == unloadResolvers_.end())
          << "unable to unload twice for plugin: "
          << nameOrPath;

        unloadResolvers_[nameOrPath]
          = std::make_unique<
              base::ManualPromiseResolver<void, base::NoReject>
            >(FROM_HERE);

        CHECK(unloadPromises_.find(nameOrPath) == unloadPromises_.end())
          << "unable to promise unload twice for plugin: "
          << nameOrPath;

        unloadPromises_[nameOrPath]
          = unloadResolvers_[nameOrPath]->promise();
      }

      {
        CHECK(loadCount_.find(nameOrPath) == loadCount_.end())
          << "unable to promise unload twice for plugin: "
          << nameOrPath;

        loadCount_[nameOrPath] = 0;
      }

      {
        CHECK(unloadCount_.find(nameOrPath) == unloadCount_.end())
          << "unable to promise unload twice for plugin: "
          << nameOrPath;

        unloadCount_[nameOrPath] = 0;
      }
    } // for

    for(std::map<std::string, PluginMetadata>::const_iterator pluginIt =
          filtered_plugins.begin()
        ; pluginIt != filtered_plugins.end()
        ; ++pluginIt)
    {
      const PluginMetadata& pluginMeta
        = (*pluginIt).second;

      const std::string& nameOrPath
        = pluginMeta.pluginName;

      // The implementation expects forward slashes as directory separators.
      DCHECK(fromNativeSeparators(nameOrPath) == nameOrPath);

      VLOG(9)
          << "plugin enabled: "
          << nameOrPath;

      VoidPromise loadDepsPromise
        = VoidPromise::CreateResolved(FROM_HERE);

      // required for `load()`
      {
        for(const std::string& dep : pluginMeta.dependsOn)
        {
          CHECK(loadPromises_.find(dep) != loadPromises_.end())
            << "unable to find dependant promise "
            << dep
            << " for plugin: "
            << nameOrPath;

          DVLOG(99)
            << "found that plugin "
            << nameOrPath
            << " depends on plugin during loading: "
            << dep;

          loadDepsPromise
            = base::Promises::All(FROM_HERE
                , std::vector<VoidPromise>{
                    loadDepsPromise
                    , loadPromises_[dep]}
              );
        }
      }

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
        = manager_->load(nameOrPath);

      if(loadState & LoadState::Loaded) {
        DVLOG(99)
          << "loaded state for plugin: "
          << nameOrPath;
      }

      if(loadState & LoadState::Static) {
        DVLOG(99)
          << "found static load state for plugin: "
          << nameOrPath;
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
            << nameOrPath
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
        = filename(nameOrPath);

      DCHECK(base::FilePath{nameOrPath}
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
      PluginPtr pluginPtr
      /// \note must be plugin name, not path to file
        = manager_->instantiate(pluginName);
      if(!pluginPtr) {
        LOG(ERROR)
          << "The requested plugin "
          << nameOrPath
          << " cannot be instantiated.";
        DCHECK(false);
        continue;
      }

      VLOG(9)
        << "=== found plugin ==";
      VLOG(9)
          << "plugin title:       "
          << pluginPtr->title();
      VLOG(9)
          << "plugin description: "
          << pluginPtr->description().substr(0, 100)
          << "...";

      if(pluginMeta.dependsOn.empty())
      {
        LOG_CALL(DVLOG(9))
          << "parallel load of plugin: "
          << nameOrPath;

        VoidPromise loadChain = pluginPtr->load()
        .ThenHere(FROM_HERE,
          loadResolvers_[nameOrPath]->GetRepeatingResolveCallback()
        )
        .ThenHere(FROM_HERE
          , base::bindCheckedOnce(
              DEBUG_BIND_CHECKS(
                PTR_CHECKER(this)
              )
              , &PluginManager::onLoaded
              , base::Unretained(this)
              , nameOrPath)
          , base::IsNestedPromise{true}
        );

        loadPromiseParallel.push_back(loadChain);
      }
      else
      {
        LOG_CALL(DVLOG(9))
          << "sequential load of plugin: "
          << nameOrPath;

        // must contain WHOLE recursive tree of dependencies
        // (and `loadDepsPromise` of each dependency)
        VoidPromise loadChain =
          loadDepsPromise
          .ThenHere(FROM_HERE
             , base::bindCheckedOnce(
                 DEBUG_BIND_CHECKS(
                   PTR_CHECKER(pluginPtr.get())
                 )
                 , &PluginType::load
                 , base::Unretained(pluginPtr.get()))
             , base::IsNestedPromise{true}
          )
          .ThenHere(FROM_HERE,
            loadResolvers_[nameOrPath]->GetRepeatingResolveCallback()
          )
          .ThenHere(FROM_HERE
            , base::bindCheckedOnce(
                DEBUG_BIND_CHECKS(
                  PTR_CHECKER(this)
                )
                , &PluginManager::onLoaded
                , base::Unretained(this)
                , nameOrPath)
            , base::IsNestedPromise{true}
          );

        loadPromiseParallel.push_back(loadChain);
      }

      loaded_plugins_.push_back(PluginWithMetadata{
        std::move(pluginPtr)
        , pluginMeta
      });
    } // for

    DCHECK(!is_initialized_)
      << "Plugin manager must be initialized once."
      << "You can unload or reload plugins at runtime.";
    is_initialized_ = true;


    return base::Promises::All(FROM_HERE, loadPromiseParallel);
  }

  // calls `unload()` for each loaded plugin
  VoidPromise shutdown()
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    TRACE_EVENT0("toplevel", "PluginManager::shutdown()");

    VLOG(9) << "(PluginManager) shutdown";

    std::vector<VoidPromise> unloadPromiseParallel;

    /// \note Need to unload plugins in order reverse to loading order.
    /// \note Destructor of ::Corrade::PluginManager::Manager
    /// also unloads all plugins.
    for (auto pluginIt = loaded_plugins_.rbegin()
         ; pluginIt != loaded_plugins_.rend()
         ; ++pluginIt)
    {
      PluginPtr& pluginPtr
        = (*pluginIt).plugin;
      DCHECK(pluginPtr);

      const PluginMetadata& pluginMeta
        = (*pluginIt).metadata;

      const std::string& nameOrPath
        = pluginMeta.pluginName;

      VoidPromise unloadRequiredByPromise
        = VoidPromise::CreateResolved(FROM_HERE);

      // required for `unload()`
      {
        for(const std::string& req
          : pluginMeta.requiredBy)
        {
          CHECK(unloadPromises_.find(req) != unloadPromises_.end())
            << "unable to find dependant promise "
            << req
            << " for plugin: "
            << nameOrPath;

          DVLOG(99)
            << "found that plugin "
            << nameOrPath
            << " required by plugin during unloading: "
            << req;

          unloadRequiredByPromise
            = base::Promises::All(FROM_HERE
                , std::vector<VoidPromise>{
                    unloadRequiredByPromise
                    , unloadPromises_[req]}
              );
        }
      }

      if(pluginMeta.requiredBy.empty())
      {
        LOG_CALL(DVLOG(9))
          << "parallel unload of plugin: "
          << nameOrPath;

        VoidPromise unloadChain = pluginPtr->unload()
        .ThenHere(FROM_HERE,
          unloadResolvers_[nameOrPath]->GetRepeatingResolveCallback()
        )
        .ThenHere(FROM_HERE
          , base::bindCheckedOnce(
              DEBUG_BIND_CHECKS(
                PTR_CHECKER(this)
              )
              , &PluginManager::onUnloaded
              , base::Unretained(this)
              , nameOrPath)
          , base::IsNestedPromise{true}
        );

        unloadPromiseParallel.push_back(unloadChain);
      }
      else
      {
        LOG_CALL(DVLOG(9))
          << "sequential unload of plugin: "
          << nameOrPath;

        // must contain WHOLE recursive tree of dependencies
        // (and `unloadRequiredByPromise` of each dependency)
        VoidPromise unloadChain =
          // make sure that we unloaded plugins that
          // require (depends on) `unloading-plugin`
          unloadRequiredByPromise
          .ThenHere(FROM_HERE
            , base::bindCheckedOnce(
                DEBUG_BIND_CHECKS(
                  PTR_CHECKER(pluginPtr.get())
                )
                , &PluginType::unload
                , base::Unretained(pluginPtr.get()))
            , base::IsNestedPromise{true}
          )
          .ThenHere(FROM_HERE,
            unloadResolvers_[nameOrPath]->GetRepeatingResolveCallback()
          )
          .ThenHere(FROM_HERE
            , base::bindCheckedOnce(
                DEBUG_BIND_CHECKS(
                  PTR_CHECKER(this)
                )
                , &PluginManager::onUnloaded
                , base::Unretained(this)
                , nameOrPath)
            , base::IsNestedPromise{true}
          );

        unloadPromiseParallel.push_back(unloadChain);
      }
    }

    return base::Promises::All(FROM_HERE, unloadPromiseParallel);
  }

  [[nodiscard]] /* do not ignore return value */
  size_t countLoadedPlugins()
  const noexcept
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return
      loaded_plugins_.size();
  }

  // write statistics etc.
  VoidPromise onLoaded(const std::string& nameOrPath)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    loadCount_[nameOrPath]++;
    UMA_HISTOGRAM_COUNTS_1000("PluginManager.onLoaded",
      loadCount_[nameOrPath]);

    return VoidPromise::CreateResolved(FROM_HERE);
  }

  // write statistics etc.
  VoidPromise onUnloaded(const std::string& nameOrPath)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    unloadCount_[nameOrPath]++;
    UMA_HISTOGRAM_COUNTS_1000("PluginManager.onUnloaded",
      unloadCount_[nameOrPath]);

    return VoidPromise::CreateResolved(FROM_HERE);
  }

private:
  bool is_initialized_ = false;

  std::map<
    std::string
    , std::unique_ptr<
        base::ManualPromiseResolver<void, base::NoReject>
      >
  > loadResolvers_;

  std::map<
    std::string
    , std::unique_ptr<
        base::ManualPromiseResolver<void, base::NoReject>
      >
  > unloadResolvers_;

  std::map<
    std::string
    , size_t
  > loadCount_;

  std::map<
    std::string
    , size_t
  > unloadCount_;

  std::map<
    std::string
    , VoidPromise
  > loadPromises_;

  std::map<
    std::string
    , VoidPromise
  > unloadPromises_;

  std::unique_ptr<
    ::Corrade::PluginManager::Manager<PluginType>
    > manager_;

  std::vector<PluginWithMetadata> loaded_plugins_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(PluginManager);
};

} // namespace plugin
