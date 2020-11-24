#pragma once

#include <base/optional.h>
#include <base/bind.h>
#include <base/macros.h>
#include <base/logging.h>

#include <basis/base_environment.hpp>

namespace backend {

// init common application systems,
// initialization order matters!
MUST_USE_RETURN_VALUE
base::Optional<int> initEnv(
  int argc
  , char* argv[]
  , ::basis::ScopedBaseEnvironment& base_env
  );

} // namespace backend
