#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <base/logging.h>

#include <exception>

namespace boost {

#ifdef BOOST_NO_EXCEPTIONS
// see https://stackoverflow.com/a/33691561
void throw_exception(const std::exception& ex)
{
  NOTREACHED()
    << "boost thrown exception: "
    << ex.what();
  /// \note application will exit, without returning.
  exit(0);
}
#endif // BOOST_NO_EXCEPTIONS

} // namespace boost
