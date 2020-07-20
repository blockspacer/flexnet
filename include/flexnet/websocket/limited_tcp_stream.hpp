#pragma once

#include <boost/beast/core/basic_stream.hpp>

namespace boost {

namespace asio {

class executor;

namespace ip {
class tcp;
} // namespace ip

} // namespace asio

namespace beast {

class simple_rate_policy;

using limited_tcp_stream
  = ::boost::beast::basic_stream<
  ::boost::asio::ip::tcp,
  ::boost::asio::executor,
  ::boost::beast::simple_rate_policy
  >;

} // namespace beast
} // namespace boost
