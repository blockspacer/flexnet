#pragma once

/**
 * @usage
  if (ec == ::boost::asio::error::connection_aborted)
  {
    LOG_ERROR_CODE(LOG(WARNING),
      "Listener failed with"
      " connection_aborted error: ", what, ec);
  }
 **/
#define LOG_ERROR_CODE(LOG_STREAM, description, what, ec) \
  LOG_STREAM  \
    << description  \
    << what  \
    << ": "  \
    << ec.message();

/**
 * @usage
  LOG_CALL(VLOG(9));
 **/
#define LOG_CALL(LOG_STREAM) \
  LOG_STREAM \
    << "called " \
    << FROM_HERE.ToString();
