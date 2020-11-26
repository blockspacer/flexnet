#pragma once

#include <flexnet/websocket/listener.hpp>
#include <flexnet/http/detect_channel.hpp>

#include <chrono>

namespace util {

/// \note Take care of thread-safety.
/// We assume that unused socket can be closed on any thread
/// i.e. we can close socket on runner of `registry` instread of
/// per-connection asio strand.
void closeSocketUnsafe(
  /// \note take care of lifetime
  ::flexnet::ws::Listener::SocketType& socket);

} // namespace util
