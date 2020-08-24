cmake_minimum_required(VERSION 3.4)

add_executable(example_server
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/src/boost_throw_exception.cc
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

# copy new resources
add_custom_command( TARGET example_server PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/
    $<TARGET_FILE_DIR:example_server>/resources )
