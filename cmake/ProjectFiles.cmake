include_guard( DIRECTORY )

list(APPEND flexnet_SOURCES
  ${flexnet_include_DIR}/websocket/limited_tcp_stream.hpp
  #
  ${flexnet_include_DIR}/websocket/listener.hpp
  ${flexnet_src_DIR}/websocket/listener.cpp
  #
  #${flexnet_include_DIR}/websocket/session.hpp
  #${flexnet_src_DIR}/websocket/session.cpp
)
