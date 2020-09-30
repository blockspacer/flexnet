#include "main_plugin_interface.hpp"
#include "main_plugin_constants.hpp"

/// \note Check version of plugin interface
/// to avoid unexpected behavior due to different interfaces.
/// \see UB and `Interface versioning in C++` on accu:
/// accu.org/journals/overload/18/100/love_1718/
REGISTER_PLUGIN(
  /*name*/
  AsioContextThreads
  , /*className*/
  /// \note Each plugin must have unique namespace
  /// to avoid unexpected behavior due to symbol collision
  plugin::asio_context_threads::MainPluginInterface
  , /*interface*/
  plugin::asio_context_threads::kPluginInterfaceVersion)
