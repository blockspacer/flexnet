#pragma once

#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp> // IWYU pragma: keep
#include <boost/beast/core.hpp>

#include <cstddef>
#include <cstdint>

namespace ECS {

// Used to schedule socket shutdown
// and (usually) free network entity after socket shutdown.
struct CloseSocket
{
  using SocketType
    = ::boost::asio::ip::tcp::socket;

  /// \todo use UnownedPtr
  SocketType* socket;
};

} // namespace ECS