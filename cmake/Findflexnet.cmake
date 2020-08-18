get_filename_component(CURRENT_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
list(APPEND CMAKE_MODULE_PATH ${CURRENT_CMAKE_DIR})

#include(CMakeFindDependencyMacro) # use find_package instead

# NOTE: some packages may be optional (platform-specific, etc.)
# find_package(... REQUIRED)
find_package(chromium_base REQUIRED)
# see https://doc.magnum.graphics/corrade/corrade-cmake.html#corrade-cmake-subproject
find_package(Corrade REQUIRED PluginManager)

list(REMOVE_AT CMAKE_MODULE_PATH -1)

if(NOT TARGET CONAN_PKG::flexnet)
  message(FATAL_ERROR "Use flexnet from conan")
endif()
set(flexnet_LIB CONAN_PKG::flexnet)
# conan package has '/include' dir
set(flexnet_HEADER_DIR
  ${CONAN_flexnet_ROOT}/include
)
# used by https://docs.conan.io/en/latest/developing_packages/workspaces.html
if(TARGET flexnet)
  # name of created target
  set(flexnet_LIB flexnet)
  # no '/include' dir on local build
  set(flexnet_HEADER_DIR
    ${CONAN_flexnet_ROOT}/include
  )
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/flexnet-config.cmake")
  # uses Config.cmake or a -config.cmake file
  # see https://gitlab.kitware.com/cmake/community/wikis/doc/tutorials/How-to-create-a-ProjectConfig.cmake-file
  # BELOW MUST BE EQUAL TO find_package(... CONFIG REQUIRED)
  # NOTE: find_package(CONFIG) not supported with EMSCRIPTEN, so use include()
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/flexnet-config.cmake)
endif()
message(STATUS "flexnet_HEADER_DIR=${flexnet_HEADER_DIR}")
