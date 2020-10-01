#include "main_plugin_constants.hpp" // IWYU pragma: associated

namespace plugin {
namespace tcp_server {

// extern
const char kDefaultIpAddr[]
  = "127.0.0.1";

// extern
const char kConfIpAddr[]
  = "ipAddr";

// extern
const int kDefaultPortNum
  = 8085;

// extern
const char kConfPortNum[]
  = "portNum";

// extern
const char kPluginInterfaceVersion[]
  = "plugin.PluginInterface/1.0";

// extern
const char kPluginName[]
  = "TcpServer";

// extern
const int kDefaultQuitDetectionFreqMillisec
  = 1000;

// extern
const char kConfQuitDetectionFreqMillisec[]
  = "quitDetectionFreqMillisec";

// extern
const int kDefaultQuitDetectionDebugTimeoutMillisec
  = 15000;

// extern
const char kConfQuitDetectionDebugTimeoutMillisec[]
  = "quitDetectionDebugTimeoutMillisec";

} // namespace tcp_server
} // namespace plugin
