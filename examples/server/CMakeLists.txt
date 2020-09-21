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
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signal_handler/signal_handler.cc
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_updater_on_sequence.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_updater_on_sequence.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_updater.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_updater.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/asio_threads_manager.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_plugin.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/network_entity_plugin.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_feature_list.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_feature_list.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_input_updater.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_input_updater.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_terminal_on_sequence.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_terminal_on_sequence.hpp
  #
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_terminal_plugin.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/console/console_terminal_plugin.hpp
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
)

target_include_directories(example_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(example_server PROPERTIES
    #FOLDER "Corrade/PluginManager/Test"
    #OUTPUT_NAME ${GAME_SERVER_EXAMPLE_TARGET_EXE}
    CXX_STANDARD 20
    CXX_EXTENSIONS OFF
    CMAKE_CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/example_server")

# POSITION_INDEPENDENT_CODE for -fPIC
set_property(TARGET example_server
  PROPERTY POSITION_INDEPENDENT_CODE ON)

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
