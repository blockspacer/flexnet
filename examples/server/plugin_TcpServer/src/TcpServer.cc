#include "plugin_interface/plugin_interface.hpp"
#include "state/app_state.hpp"
#include "registry/main_loop_registry.hpp"

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

#include <basis/scoped_sequence_context_var.hpp>
#include <basis/scoped_log_run_time.hpp>
#include <basis/promise/post_promise.h>
#include <basis/ECS/sequence_local_context.hpp>
#include <basis/unowned_ref.hpp>
#include <basis/status/statusor.hpp>

#include <entt/entity/registry.hpp>
#include <entt/signal/dispatcher.hpp>
#include <entt/entt.hpp>

#include <thread>

namespace plugin {
namespace tcp_server {

class TcpServerPlugin
{
 public:
  using VoidPromise
    = base::Promise<void, base::NoReject>;

  using StatusPromise
    = base::Promise<::util::Status, base::NoReject>;

 public:
  TcpServerPlugin();

  ~TcpServerPlugin();

 private:
  SET_WEAK_POINTERS(TcpServerPlugin);

  // Same as `base::MessageLoop::current()->task_runner()`
  // during class construction
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_
    SET_STORAGE_THREAD_GUARD(MEMBER_GUARD(mainLoopRunner_));

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(TcpServerPlugin);
};

TcpServerPlugin::TcpServerPlugin()
  : ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_ptr_factory_(COPIED(this)))
  , ALLOW_THIS_IN_INITIALIZER_LIST(
      weak_this_(
        weak_ptr_factory_.GetWeakPtr()))
  , mainLoopRunner_{
      base::MessageLoop::current()->task_runner()}
{
  LOG_CALL(DVLOG(99));

  DETACH_FROM_SEQUENCE(sequence_checker_);
}

TcpServerPlugin::~TcpServerPlugin()
{
  LOG_CALL(DVLOG(99));

  DCHECK_RUN_ON(&sequence_checker_);
}

class TcpServer
  final
  : public ::plugin::PluginInterface {
 public:
  explicit TcpServer(
    ::plugin::AbstractManager& manager
    , const std::string& pluginName)
    : ::plugin::PluginInterface{manager, pluginName}
    , mainLoopRunner_(base::MessageLoop::current()->task_runner())
  {
    LOG_CALL(DVLOG(99));

    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~TcpServer()
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

    TRACE_EVENT0("headless", "plugin::TcpServer::load()");

    return
      base::PostPromise(FROM_HERE
        , UNOWNED_LIFETIME(mainLoopRunner_.get())
        , base::BindOnce(
          [
          ](
          ){
             LOG_CALL(DVLOG(99))
              << " TcpServer starting...";
          })
      );
  }

  VoidPromise unload() override
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    DCHECK(mainLoopRunner_->RunsTasksInCurrentSequence());

    TRACE_EVENT0("headless", "plugin::TcpServer::unload()");

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
              << " TcpServer terminating...";
          })
      );
  }

private:
  scoped_refptr<base::SingleThreadTaskRunner> mainLoopRunner_;

  TcpServerPlugin tcpServerPlugin_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(TcpServer);
};

} // namespace tcp_server
} // namespace plugin

REGISTER_PLUGIN(/*name*/ TcpServer
    , /*className*/ plugin::tcp_server::TcpServer
    // plugin interface version checks to avoid unexpected behavior
    , /*interface*/ "plugin.PluginInterface/1.0")
