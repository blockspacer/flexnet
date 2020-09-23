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

///////////////

#include "plugin_manager/plugin_interface.hpp"
#include "plugin_manager/plugin_manager.hpp"
#include "state/app_state.hpp"
#include "signal_handler/signal_handler.hpp"
#include "console_terminal/console_terminal_plugin.hpp"
#include "console_terminal/console_terminal_on_sequence.hpp"
#include "net/asio_threads_manager.hpp"
#include "tcp_entity_allocator.hpp"

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>
#include <flexnet/ECS/tags.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/optional.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>
#include <base/threading/thread_collision_warner.h>

#include <basis/task/task_util.hpp>
#include <basis/checked_optional.hpp>
#include <basis/lock_with_check.hpp>
#include <basis/task/periodic_validate_until.hpp>
#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>
#include <basis/ECS/asio_registry.hpp>
#include <basis/ECS/simulation_registry.hpp>
#include <basis/ECS/global_context.hpp>
#include <basis/move_only.hpp>
#include <basis/unowned_ptr.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/base_environment.hpp>
#include <basis/task/periodic_task_executor.hpp>
#include <basis/promise/post_promise.h>
#include <basis/task/periodic_check.hpp>
#include <basis/strong_alias.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <memory>
#include <chrono>

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

#if 0
struct ServerStartOptions
{
  const std::string ip_addr = "127.0.0.1";
  const unsigned short port_num = 8085;
};
#endif // 0

class NetworkEntity
  final
  : public ::plugin::PluginInterface
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

  using EndpointType
    = ws::Listener::EndpointType;

 public:
  explicit NetworkEntity(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);
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

    TRACE_EVENT0("headless", "plugin::NetworkEntity::load()");

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
              << " NetworkEntity starting...";
          })
      )
      .ThenOn(mainLoopRunner_
        , FROM_HERE
        , base::BindOnce(
            &NetworkEntityPlugin::unload
            , base::Unretained(&networkEntityPlugin_)
        )
        , /*nestedPromise*/ true
      );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::NetworkEntity::unload()");

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
              << " NetworkEntity terminating...";
          })
      )
      .ThenOn(mainLoopRunner_
        , FROM_HERE
        , base::BindOnce(
            &NetworkEntityPlugin::load
            , base::Unretained(&networkEntityPlugin_)
            , REFERENCED(asioRegistry_)
            , REFERENCED(ioc_)
        )
        , /*nestedPromise*/ true
      );
  }

private:
  basis::ScopedLogRunTime scopedLogRunTime_{};

  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  /// \todo use plugin loader
  NetworkEntityPlugin networkEntityPlugin_;
    /// \todo
    //GUARDED_BY(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(NetworkEntity);
};

} // namespace plugin

REGISTER_PLUGIN(/*name*/ NetworkEntity
    , /*className*/ plugin::NetworkEntity
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
