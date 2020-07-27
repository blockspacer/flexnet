include_guard( DIRECTORY )

list(APPEND flexnet_SOURCES
  ${flexnet_include_DIR}/http/detect_channel.hpp
  ${flexnet_src_DIR}/http/detect_channel.cpp
  #
  ${flexnet_include_DIR}/websocket/listener.hpp
  ${flexnet_src_DIR}/websocket/listener.cpp
  #
  #${flexnet_include_DIR}/websocket/websocket_channel.hpp
  #${flexnet_src_DIR}/websocket/websocket_channel.cpp
  #
  ${flexnet_include_DIR}/util/mime_type.hpp
  ${flexnet_src_DIR}/util/mime_type.cpp
  #
  ${flexnet_include_DIR}/util/limited_tcp_stream.hpp
  #
  ${flexnet_include_DIR}/util/macros.hpp
  #
  ${flexnet_include_DIR}/util/move_only.hpp
  #
  ${flexnet_include_DIR}/util/unowned_ptr.hpp
  #
  ${flexnet_include_DIR}/util/unowned_ref.hpp
  #
  ${flexnet_include_DIR}/util/promise_collection.hpp
)
