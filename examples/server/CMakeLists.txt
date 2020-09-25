cmake_minimum_required(VERSION 3.4)

macro(add_server_lib LIBRARY_NAME)
  # library
  add_library(${LIBRARY_NAME}
    ${ARGN}
  )

  target_link_libraries(${LIBRARY_NAME} PRIVATE
    ${LIB_NAME}
    CONAN_PKG::corrade
    Corrade::PluginManager
  )

  set_target_properties(${LIBRARY_NAME} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CXX_STANDARD 20
      CXX_EXTENSIONS OFF
      CMAKE_CXX_STANDARD_REQUIRED ON
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/${LIBRARY_NAME}"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/${LIBRARY_NAME}"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/${LIBRARY_NAME}")

  # POSITION_INDEPENDENT_CODE for -fPIC
  set_property(TARGET ${LIBRARY_NAME}
    PROPERTY POSITION_INDEPENDENT_CODE ON)

  target_compile_options(${LIBRARY_NAME} PRIVATE
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
    ${LIBRARY_NAME}
    ENABLE ALL
    DISABLE Annoying)
endmacro(add_server_lib)

add_server_lib(PluginInterfaceSharedLib
  ${CMAKE_CURRENT_SOURCE_DIR}/PluginInterfaceSharedLib/plugin_interface/plugin_interface.hpp
)

target_include_directories(PluginInterfaceSharedLib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/PluginInterfaceSharedLib/
)

add_server_lib(ServerSharedLib
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib/boost_throw_exception.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib/state/app_state.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib/state/app_state.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib/registry/main_loop_registry.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib/registry/main_loop_registry.hpp
)

target_include_directories(ServerSharedLib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/ServerSharedLib
)

add_server_lib(ConsoleTerminalSharedLib
  ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleTerminalSharedLib/console_terminal/console_dispatcher.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleTerminalSharedLib/console_terminal/console_feature_list.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleTerminalSharedLib/console_terminal/console_feature_list.hpp
)

target_include_directories(ConsoleTerminalSharedLib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/ConsoleTerminalSharedLib
)

# executable
add_executable(server_executable
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/init_env.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/init_env.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_entity_allocator.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_entity_allocator.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/server_run_loop_state.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/server_run_loop_state.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_manager/plugin_manager.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_manager/plugin_manager.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.hpp
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

target_link_libraries(server_executable PRIVATE
  ${LIB_NAME}
  CONAN_PKG::corrade
  Corrade::PluginManager
  ServerSharedLib
  PluginInterfaceSharedLib
)

target_include_directories(server_executable PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(server_executable PROPERTIES
    # ENABLE_EXPORTS for -rdynamic
    # i.e. export symbols from the executables
    # required for plugin system
    # see https://stackoverflow.com/a/8626922
    ENABLE_EXPORTS ON
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_EXTENSIONS OFF
    CMAKE_CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable")

# POSITION_INDEPENDENT_CODE for -fPIC
set_property(TARGET server_executable
  PROPERTY POSITION_INDEPENDENT_CODE ON)

set(GENERATE_CORRADE_PLUGIN_IMPORT "")
set(GENERATE_CORRADE_PLUGIN_EJECT "")

target_compile_options(server_executable PRIVATE
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
  server_executable
  ENABLE ALL
  DISABLE Annoying)

# copy new resources
add_custom_command( TARGET server_executable PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/
    $<TARGET_FILE_DIR:server_executable>/resources )

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
    PluginInterfaceSharedLib
  )
  set_target_properties(${PLUGIN_NAME} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CXX_STANDARD 20
      CXX_EXTENSIONS OFF
      # Plugins don't have any prefix (e.g. 'lib' on Linux)
      PREFIX ""
      CMAKE_CXX_STANDARD_REQUIRED ON
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable/plugins"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable/plugins"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable/plugins")

  # POSITION_INDEPENDENT_CODE for -fPIC
  set_property(TARGET ${PLUGIN_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

  #
  if(${PLUGIN_NAME}_STATIC)
    target_link_libraries(server_executable PRIVATE
      ${PLUGIN_NAME}
    )
    #
    set(GENERATE_CORRADE_PLUGIN_IMPORT
      "${GENERATE_CORRADE_PLUGIN_IMPORT};CORRADE_PLUGIN_IMPORT(${PLUGIN_NAME});")
    #
    set(GENERATE_CORRADE_PLUGIN_EJECT
      "${GENERATE_CORRADE_PLUGIN_EJECT};CORRADE_PLUGIN_EJECT(${PLUGIN_NAME});")
  else()
    # NOTE: only dynamic plugins can have dynamic config file
    execute_process(COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_RELATIVE_DIR}/conf/${PLUGIN_NAME}.conf
      ${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/server_executable/plugins/${PLUGIN_NAME}.conf
    )
  endif(${PLUGIN_NAME}_STATIC)
endmacro(add_server_plugin)

add_server_plugin(ExtraLogging
  plugin_ExtraLogging
  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_ExtraLogging/src/ExtraLogging.cc
)

add_server_plugin(BasicConsoleCommands
  plugin_BasicConsoleCommands
  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_BasicConsoleCommands/src/BasicConsoleCommands.cc
)

target_link_libraries(BasicConsoleCommands PRIVATE
  ConsoleTerminalSharedLib
  ServerSharedLib
)

add_server_plugin(ConsoleTerminal
  plugin_ConsoleTerminal
  ${CMAKE_CURRENT_SOURCE_DIR}/plugin_ConsoleTerminal/src/ConsoleTerminal.cc
)

target_link_libraries(ConsoleTerminal PRIVATE
  ConsoleTerminalSharedLib
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

message(STATUS
  "GENERATE_CORRADE_PLUGIN_IMPORT=${GENERATE_CORRADE_PLUGIN_IMPORT}")

message(STATUS
  "GENERATE_CORRADE_PLUGIN_EJECT=${GENERATE_CORRADE_PLUGIN_EJECT}")

set(STATIC_PLUGINS_HEADER_LOCATION
  ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/static_plugins.hpp)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/codegen/static_plugins.hpp.in
  ${STATIC_PLUGINS_HEADER_LOCATION})

set_source_files_properties(${STATIC_PLUGINS_HEADER_LOCATION}
  PROPERTIES GENERATED 1)

target_sources(server_executable PRIVATE
  ${STATIC_PLUGINS_HEADER_LOCATION}
)
