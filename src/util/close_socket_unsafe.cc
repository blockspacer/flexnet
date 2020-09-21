#include <flexnet/util/close_socket_unsafe.hpp> // IWYU pragma: associated
#include <flexnet/ECS/tags.hpp>
#include <flexnet/ECS/components/tcp_connection.hpp>

#include <base/logging.h>
#include <base/trace_event/trace_event.h>
#include <base/guid.h>

namespace util {

using Listener
  = flexnet::ws::Listener;

void closeSocketUnsafe(
  /// \note take care of lifetime
  Listener::SocketType& socket)
{
  if(socket.is_open())
  {
    DVLOG(99)
      << "shutdown of asio socket...";

    boost::beast::error_code ec;

    // `shutdown_both` disables sends and receives
    // on the socket.
    /// \note Call before `socket.close()` to ensure
    /// that any pending operations on the socket
    /// are properly cancelled and any buffers are flushed
    socket.shutdown(
      boost::asio::ip::tcp::socket::shutdown_both, ec);

    if (ec) {
      LOG(WARNING)
        << "error during shutdown of asio socket: "
        << ec.message();
    }

    // Close the socket.
    // Any asynchronous send, receive
    // or connect operations will be cancelled
    // immediately, and will complete with the
    // boost::asio::error::operation_aborted error.
    /// \note even if the function indicates an error,
    /// the underlying descriptor is closed.
    /// \note For portable behaviour with respect to graceful closure of a
    /// connected socket, call shutdown() before closing the socket.
    socket.close(ec);

    if (ec) {
      LOG(WARNING)
        << "error during close of asio socket: "
        << ec.message();
    }
  } else {
    DVLOG(99)
      << "unable to shutdown already closed asio socket...";
  }
}

} // namespace util
