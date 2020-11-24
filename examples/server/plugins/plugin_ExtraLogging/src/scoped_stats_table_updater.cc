#include "scoped_stats_table_updater.hpp" // IWYU pragma: associated

#include <basis/scoped_sequence_context_var.hpp>
#include <basis/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>
#include <basis/task/periodic_check.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/strong_types/strong_alias.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>
#include <iostream>

namespace plugin {
namespace extra_logging {

ScopedStatsTableUpdater::ScopedStatsTableUpdater()
{
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

ScopedStatsTableUpdater::~ScopedStatsTableUpdater()
{
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ::base::StatsTable table_{
    "app_stats_table" // name
    , 50 // max_threads
    , 1000 // max_counters
  };

  table_.set_current(&table_);

  table_.RegisterCurrentThread("server_main_thread");

  ::base::Time app_epoch;
  {
    static const char kAppEpochStr[]
      = "Sun, 01 May 2000 12:00:00 GMT";
    const bool appEpochOk
      = ::base::Time::FromString(kAppEpochStr, &app_epoch);
    DCHECK(appEpochOk);
  }

  /// \note save time of last launch attempt into shared memory file
  ::base::Time prev_last_launch;
  {
    VLOG(9)
      << "time now: "
      << ::base::Time::Now();
    const std::string counter_name = "appserver.last_launch_time";
    int counter_id
      = table_.FindOrAddCounter(counter_name);
    int* counter_loc
      = table_.GetLocation(counter_id, table_.GetSlot());
    DCHECK(counter_loc);
    if(*counter_loc) {
      prev_last_launch
        = app_epoch + ::base::TimeDelta::FromSeconds(*counter_loc);
      LOG(INFO)
        << "last launch time (seconds precision): "
        << prev_last_launch;
    }
    (*counter_loc) = (base::Time::Now() - app_epoch).InSeconds();
  }

  /// \note saves number of launch attempts into shared memory file,
  /// but only for RECENT launch attempts,
  /// see |kMinutesToResetLaunchCounter|
  {
    const std::string counter_name = "appserver.recent_launch_counter";
    int counter_id
      = table_.FindOrAddCounter(counter_name);
    int* counter_loc
      = table_.GetLocation(counter_id, table_.GetSlot());
    DCHECK(counter_loc);
    /// \note counter will NOT be reset it app launches
    /// more frequently than |kMinutesToResetLaunchCounter|
    /// i.e. if app restarts every second,
    /// than counter will increase forever
    static const int kMinutesToResetLaunchCounter
      = 5;
    const auto minutesSinceLastLaunch
      = (base::Time::Now() - prev_last_launch).InMinutes();
    if(minutesSinceLastLaunch > kMinutesToResetLaunchCounter) {
      (*counter_loc) = 0;
      VLOG(9)
        << "launch counter set to 1";
    }
    (*counter_loc) += 1;
  }

  // Dump the stats table.
  LOG(INFO) << "<stats>";
  int counter_max = table_.GetMaxCounters();
  for (int index=0; index < counter_max; index++) {
    std::string name(table_.GetRowName(index));
    if (name.length() > 0) {
      int value = table_.GetRowValue(index);
      LOG(INFO) << name << "\t" << value;
    }
  }
  LOG(INFO) << "</stats>";
}

} // namespace extra_logging
} // namespace plugin
