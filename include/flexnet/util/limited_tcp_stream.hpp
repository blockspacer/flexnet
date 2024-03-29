#pragma once

#include <boost/beast/core.hpp>

namespace boost {

namespace asio {

class executor;

namespace ip {
class tcp;
} // namespace ip

} // namespace asio

namespace beast {

// stream with custom rate limiter
using limited_tcp_stream
  = ::boost::beast::basic_stream<
  ::boost::asio::ip::tcp,
  ::boost::asio::executor,
  ::boost::beast::simple_rate_policy
  >;

} // namespace beast
} // namespace boost
