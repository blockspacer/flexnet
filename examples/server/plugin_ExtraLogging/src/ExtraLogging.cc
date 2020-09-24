#include "plugin_manager/plugin_interface.hpp"

#include <base/logging.h>
#include <base/cpu.h>
#include <base/command_line.h>
#include <base/debug/alias.h>
#include <base/debug/stack_trace.h>
#include <base/memory/ptr_util.h>
#include <base/sequenced_task_runner.h>
#include <base/strings/string_util.h>
#include <base/trace_event/trace_event.h>
#include <base/strings/string_number_conversions.h>
#include <base/numerics/safe_conversions.h>

#include <basis/scoped_log_run_time.hpp>

#include <thread>

namespace {

/// \todo make configurable in plugin [configuration]
static constexpr int kDefaultMinCores
  = 4;

static constexpr char kConfMinCoresBeforeWarn[]
  = "minCoresBeforeWarn";

static void checkNumOfCores(const size_t minCores)
{
  unsigned int coresTotal
    = std::thread::hardware_concurrency();

  LOG(INFO)
    << "Number of cores: "
    << coresTotal;

  if (coresTotal < minCores)
  {
    LOG(INFO)
      << "Too low number of cores!"
         " Prefer servers with at least "
      << minCores
      << " cores";
  }
}

static void displayMachineInfo()
{
  base::CPU cpu;
  LOG(INFO)
    << "has_sse:"
    << cpu.has_sse();

  LOG(INFO)
    << "has_avx:"
    << cpu.has_avx();
}

} // namespace

namespace plugin {

class ExtraLogging
  final
  : public ::plugin::PluginInterface {
 public:
  explicit ExtraLogging(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
  {
    LOG_CALL(DVLOG(99));

    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~ExtraLogging()
  {
    LOG_CALL(DVLOG(99));

    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  std::string title() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("title");
  }

  std::string author() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("author");
  }

  std::string description() const override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    return metadata()->data().value("description");
  }

  VoidPromise load() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::ExtraLogging::load()");

    DLOG(INFO)
      << "loaded plugin with title = "
      << title()
      << " and description = "
      << description().substr(0, 100)
      << "...";
    displayMachineInfo();

    int confMinCores
      = kDefaultMinCores;

    if(metadata()->configuration().hasValue(kConfMinCoresBeforeWarn))
    {
      base::StringToInt(
        metadata()->configuration().value(kConfMinCoresBeforeWarn)
        , &confMinCores);
    }

    checkNumOfCores(
      /// \note Crash if out of range.
      base::checked_cast<size_t>(confMinCores));

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
             LOG_CALL(DVLOG(99))
              << " ExtraLogging starting...";
          })
      );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::ExtraLogging::unload()");

    DLOG(INFO)
      << "unloaded plugin with title = "
      << title()
      << " and description = "
      << description().substr(0, 100)
      << "...";

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
             LOG_CALL(DVLOG(99))
              << " ExtraLogging terminating...";
          })
      );
  }

private:
  basis::ScopedLogRunTime scopedLogRunTime_{};

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  DISALLOW_COPY_AND_ASSIGN(ExtraLogging);
};

} // namespace plugin

REGISTER_PLUGIN(/*name*/ ExtraLogging
    , /*className*/ plugin::ExtraLogging
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
