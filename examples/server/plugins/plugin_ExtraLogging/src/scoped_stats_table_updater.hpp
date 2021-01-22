#pragma once

#include <base/logging.h>
#include <base/cpu.h>
#include <base/macros.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/strings/string_util.h>
#include <base/trace_event/trace_event.h>
#include <base/strings/string_number_conversions.h>
#include <base/numerics/safe_conversions.h>
#include <basis/tracing/stats_table.hpp>

#include <basis/log/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/status/statusor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>

namespace plugin {
namespace extra_logging {

class MainPluginInterface;

// Will write data into |base::StatsTable| on scope exit
// and print previous saved values
// (like time elapsed from last launch)
// |base::StatsTable| saves metrics into shared memory file
class ScopedStatsTableUpdater
{
public:
  ScopedStatsTableUpdater();

  ~ScopedStatsTableUpdater();

private:
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ScopedStatsTableUpdater);
};

} // namespace extra_logging
} // namespace plugin
