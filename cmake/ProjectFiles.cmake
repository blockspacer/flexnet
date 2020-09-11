include_guard( DIRECTORY )

list(APPEND flexnet_SOURCES
  ${flexnet_include_DIR}/http/detect_channel.hpp
  ${flexnet_src_DIR}/http/detect_channel.cpp
  #
  ${flexnet_include_DIR}/http/http_channel.hpp
  ${flexnet_src_DIR}/http/http_channel.cpp
  #
  ${flexnet_include_DIR}/websocket/listener.hpp
  ${flexnet_src_DIR}/websocket/listener.cpp
  #
  ${flexnet_include_DIR}/websocket/ws_channel.hpp
  ${flexnet_src_DIR}/websocket/ws_channel.cpp
  #
  #${flexnet_include_DIR}/websocket/websocket_channel.hpp
  #${flexnet_src_DIR}/websocket/websocket_channel.cpp
  #
  ${flexnet_include_DIR}/util/mime_type.hpp
  ${flexnet_src_DIR}/util/mime_type.cpp
  #
  ${flexnet_include_DIR}/util/limited_tcp_stream.hpp
  #
  ${flexnet_include_DIR}/util/lock_with_check.hpp
  ${flexnet_src_DIR}/util/lock_with_check.cpp
  #
  ${flexnet_include_DIR}/util/periodic_validate_until.hpp
  ${flexnet_src_DIR}/util/periodic_validate_until.cpp
  #
  ${flexnet_include_DIR}/util/scoped_sequence_context_var.hpp
  ${flexnet_src_DIR}/util/scoped_sequence_context_var.cpp
)
