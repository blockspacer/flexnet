#pragma once

#include <base/feature_list.h>

namespace backend {

// USAGE: ./appexe --enable-features=console_terminal,other1,other2
constexpr char kFeatureConsoleTerminalName[]
  = "console_terminal";

extern const ::base::Feature kFeatureConsoleTerminal;

} // namespace backend
