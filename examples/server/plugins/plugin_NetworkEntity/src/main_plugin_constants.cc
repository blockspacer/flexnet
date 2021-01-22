#include "main_plugin_constants.hpp" // IWYU pragma: associated

namespace plugin {
namespace network_entity {

// extern
const char kPluginInterfaceVersion[]
  = "plugin.PluginInterface/1.0";

// extern
const char kPluginName[]
  = "NetworkEntity";

// extern
const int kDefaultEntityUpdateFreqMillisec
  = 1000;

// extern
const char kConfEntityUpdateFreqMillisec[]
  = "entityUpdateFreqMillisec";

const char kWarnBigUpdateQueueSize[] =
    "network-entity-updater-warn-big-update-queue-size";

const char kWarnBigUpdateQueueFreqMs[] =
    "network-entity-updater-warn-big-update-queue-freq-ms";

} // namespace network_entity
} // namespace plugin
