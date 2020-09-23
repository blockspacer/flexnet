cmake_minimum_required(VERSION 3.4)

add_executable(example_server
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/boost_throw_exception.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/init_env.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/init_env.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_entity_allocator.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_entity_allocator.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/example_server.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/example_server.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/state/app_state.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/state/app_state.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_manager/plugin_manager.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_manager/plugin_manager.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_manager/plugin_interface.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_feature_list.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_feature_list.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_input_updater.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_input_updater.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_terminal_on_sequence.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_terminal_on_sequence.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_terminal_plugin.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console_terminal/console_terminal_plugin.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/cleanup.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/cleanup.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/unused.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/unused.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/ssl_detect_result.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/ssl_detect_result.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/accept_connection_result.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ECS/systems/accept_connection_result.hpp
)

target_link_libraries(example_server PRIVATE
  ${LIB_NAME}
  CONAN_PKG::corrade
  Corrade::PluginManager
)

target_include_directories(example_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(example_server PROPERTIES
    # ENABLE_EXPORTS for -rdynamic
    # i.e. export symbols from the executables
    # required for plugin system
    # see https://stackoverflow.com/a/8626922
    ENABLE_EXPORTS ON
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_EXTENSIONS OFF
    CMAKE_CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server")

# POSITION_INDEPENDENT_CODE for -fPIC
set_property(TARGET example_server
  PROPERTY POSITION_INDEPENDENT_CODE ON)

set(GENERATE_CORRADE_PLUGIN_IMPORT "")
set(GENERATE_CORRADE_PLUGIN_EJECT "")

target_compile_options(example_server PRIVATE
  # NOTE: explicitly select the "C++ Core Check Lifetime Rules" (or "Microsoft All Rules") in order to enable the lifetime checks.
  # see https://devblogs.microsoft.com/cppblog/c-core-guidelines-checker-in-visual-studio-2017/
  # see https://www.modernescpp.com/index.php/c-core-guidelines-lifetime-safety
  $<$<CXX_COMPILER_ID:MSVC>:
    /W3 # Set warning level
    /Wall
    /analyze
  >
  # see https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html
  $<$<CXX_COMPILER_ID:GNU>:
    ${GCCAndClangErrorFlags}
    ${GCCAndClangWarningFlags}
    -Wall
    -W
    -Wextra
    -Wpedantic
    -Wdeprecated-register
    -Wnon-virtual-dtor
  >
  $<$<CXX_COMPILER_ID:Clang>:
    ${GCCAndClangErrorFlags}
    ${GCCAndClangWarningFlags}
    ${ClangErrorFlags}
    # see https://pspdfkit.com/blog/2020/the-cpp-lifetime-profile/
    # TODO: only special branch of Clang currently https://github.com/mgehre/llvm-project
    #-Wlifetime
    # see http://clang.llvm.org/docs/ThreadSafetyAnalysis.html
    # see https://github.com/isocpp/CppCoreGuidelines/blob/master/docs/Lifetime.pdf
    -Wthread-safety-analysis
    -Wall
    -W
    -Wextra
    -Wpedantic
    -Wdeprecated-register
    -Wnon-virtual-dtor
    # Negative requirements are an experimental feature
    # which will produce many warnings in existing code
    -Wno-thread-safety-negative
  >
)

# # Helper that can set default warning flags for you
target_set_warnings( # from cmake_helper_utils (conan package)
  example_server
  ENABLE ALL
  DISABLE Annoying)

# copy new resources
add_custom_command( TARGET example_server PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/
    $<TARGET_FILE_DIR:example_server>/resources )

macro(add_server_plugin PLUGIN_NAME PLUGIN_RELATIVE_DIR)
  set(${PLUGIN_NAME}_STATIC FALSE)
  if(ENABLE_MSAN OR ENABLE_TSAN OR ENABLE_ASAN OR ENABLE_UBSAN)
    set(${PLUGIN_NAME}_STATIC TRUE)
    message(STATUS "added static plugin: ${PLUGIN_NAME}")
  endif()
  if(${PLUGIN_NAME}_STATIC)
    set(metadata_file "${PLUGIN_RELATIVE_DIR}/conf/${PLUGIN_NAME}.conf")
    corrade_add_static_plugin(${PLUGIN_NAME}
      ${CMAKE_CURRENT_BINARY_DIR}
      ${metadata_file}
      ${ARGN})
  else()
    add_library(${PLUGIN_NAME} SHARED
      ${ARGN}
    )
    target_compile_definitions(${PLUGIN_NAME} PRIVATE
      # see https://github.com/mosra/corrade/blob/af9d4216f07307a2dff471664eed1e50e180568b/modules/UseCorrade.cmake#L568
      CORRADE_DYNAMIC_PLUGIN=1
    )
  endif(${PLUGIN_NAME}_STATIC)
  #
  target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_RELATIVE_DIR}
  )
  #
  target_link_libraries(${PLUGIN_NAME} PRIVATE
    ${LIB_NAME}
    CONAN_PKG::corrade
    Corrade::PluginManager
    ## 3dparty libs
    ${USED_3DPARTY_LIBS}
    ## system libs
    ${USED_SYSTEM_LIBS}
    ${base_LIB}
    ${build_util_LIB}
  )
  set_target_properties(${PLUGIN_NAME} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CXX_STANDARD 20
      CXX_EXTENSIONS OFF
      # Plugins don't have any prefix (e.g. 'lib' on Linux)
      PREFIX ""
      CMAKE_CXX_STANDARD_REQUIRED ON
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server/plugins"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server/plugins"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server/plugins")

  # POSITION_INDEPENDENT_CODE for -fPIC
  set_property(TARGET ${PLUGIN_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

  #
  if(${PLUGIN_NAME}_STATIC)
    target_link_libraries(example_server PRIVATE
      ${PLUGIN_NAME}
    )
    #
    set(GENERATE_CORRADE_PLUGIN_IMPORT "\
${GENERATE_CORRADE_PLUGIN_IMPORT}\
CORRADE_PLUGIN_IMPORT(${PLUGIN_NAME});")
    #
    set(GENERATE_CORRADE_PLUGIN_EJECT "\
${GENERATE_CORRADE_PLUGIN_EJECT}\
CORRADE_PLUGIN_EJECT(${PLUGIN_NAME});")
  else()
    # NOTE: only dynamic plugins can have dynamic config file
    execute_process(COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_RELATIVE_DIR}/conf/${PLUGIN_NAME}.conf
      ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server/plugins/${PLUGIN_NAME}.conf
    )
  endif(${PLUGIN_NAME}_STATIC)
endmacro(add_server_plugin)

add_server_plugin(ExtraLogging
  plugin_ExtraLogging
  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_ExtraLogging/src/ExtraLogging.cc
)

#add_server_plugin(NetworkEntity
#  plugin_NetworkEntity
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/NetworkEntity.cc
#  #
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_updater_on_sequence.cc
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_updater_on_sequence.hpp
#  #
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_updater.cc
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_updater.hpp
#  #
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_plugin.cc
#  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_NetworkEntity/src/network_entity_plugin.hpp
#)

target_compile_definitions(example_server PRIVATE
  # see https://github.com/mosra/corrade/blob/af9d4216f07307a2dff471664eed1e50e180568b/modules/UseCorrade.cmake#L568
  GENERATE_CORRADE_PLUGIN_IMPORT=${GENERATE_CORRADE_PLUGIN_IMPORT}
  GENERATE_CORRADE_PLUGIN_EJECT=${GENERATE_CORRADE_PLUGIN_EJECT}
)
