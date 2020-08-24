cmake_minimum_required(VERSION 3.4)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/parse_version_file.cmake)

# project() must be before checks, see https://stackoverflow.com/a/26437667/10904212
set(PROJECT_NAME "flexnet")
project(${PROJECT_NAME}
  VERSION ${PROJECT_VERSION}
  LANGUAGES CXX C)

set(LIB_NAME ${PROJECT_NAME})

# ------------------------- External CMake includes ----------------------------

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_externals.cmake)

# ------------------------------- Options --------------------------------------

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Options.cmake)

# --------------------------- conan configuration ------------------------------

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch_cmake_utils.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fetch_conan_auto_install.cmake)

option(CONAN_AUTO_INSTALL "Let CMake call conan install automatically"
  OFF
)
if (CONAN_AUTO_INSTALL)
  set(CONAN_PROFILE
      "clang"
      CACHE STRING "Conan profile to use during installation")
  if (NOT CMAKE_BUILD_TYPE MATCHES "Debug" )
    set(conan_build_type "Release")
  else()
    set(conan_build_type "Debug")
  endif()
  # No --build=missing cause llvm requires long build.
  # You can use `EXTRA_CONAN_AUTO_INSTALL_OPTIONS` like so:
  # cmake .. -DCONAN_AUTO_INSTALL=ON -DEXTRA_CONAN_AUTO_INSTALL_OPTIONS="--build missing" -DCMAKE_BUILD_TYPE=Debug
  conan_auto_install(
    CONAN_OPTIONS "--profile=${CONAN_PROFILE} -s build_type=${conan_build_type} -s llvm_tools:build_type=Release -o openssl:shared=True ${EXTRA_CONAN_AUTO_INSTALL_OPTIONS}"
    #FORCE
  )
endif()

if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/conanbuildinfo.cmake")
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_BINARY_DIR}/)
  include(${CMAKE_CURRENT_BINARY_DIR}/conanbuildinfo.cmake)
  include(${CMAKE_CURRENT_BINARY_DIR}/conan_paths.cmake OPTIONAL)
  conan_basic_setup(
    # prevent conan_basic_setup from resetting cmake variables
    TARGETS
    KEEP_RPATHS
    # see https://github.com/conan-io/conan/issues/6012
    NO_OUTPUT_DIRS
    )
else()
  message (FATAL_ERROR "must use conan")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

# Generate clang compilation database
# see https://stackoverflow.com/a/31086619/10904212
#set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(cmake_platform_detection REQUIRED)
run_cmake_platform_detection()

find_package(cmake_build_options REQUIRED)
setup_default_build_type(RELEASE)
setup_cmake_build_options(RELEASE DEBUG)

message(STATUS "Compiler ${CMAKE_CXX_COMPILER}, version: ${CMAKE_CXX_COMPILER_VERSION}")

find_package(cmake_helper_utils REQUIRED)

if(ENABLE_MSAN OR ENABLE_TSAN OR ENABLE_ASAN OR ENABLE_UBSAN)
  find_package(cmake_sanitizers REQUIRED)
endif()

if(ENABLE_MSAN)
  add_msan_flags()
endif(ENABLE_MSAN)

if(ENABLE_TSAN)
  add_tsan_flags()
endif(ENABLE_TSAN)

if(ENABLE_ASAN)
  add_asan_flags()
endif(ENABLE_ASAN)

if(ENABLE_UBSAN)
  add_ubsan_flags()
endif(ENABLE_UBSAN)

if(COMPILE_WITH_LLVM_TOOLS)
  # force change CMAKE_*_COMPILER and CMAKE_LINKER to clang from conan
  compile_with_llvm_tools() # from cmake_helper_utils (conan package)
endif(COMPILE_WITH_LLVM_TOOLS)

set_project_version(0 0 1) # from Utils.cmake

check_cmake_build_type_selected() # from Utils.cmake

enable_colored_diagnostics() # from Utils.cmake

print_cmake_system_info() # from Utils.cmake

check_supported_os() # from Utils.cmake

# Keep symbols for JIT resolution
set(LLVM_NO_DEAD_STRIP 1)

#if(NOT TARGET CONAN_PKG::clang_folly_conan)
#  message(FATAL_ERROR "Use clang_folly_conan from conan")
#endif()

if(NOT TARGET CONAN_PKG::chromium_build_util)
  message(FATAL_ERROR "Use chromium_build_util from conan")
endif()

find_package(chromium_build_util REQUIRED)
#
if(TARGET chromium_build_util::chromium_build_util-static)
  set(build_util_LIB "chromium_build_util::chromium_build_util-static")
else()
  message(FATAL_ERROR "not supported: using system provided chromium_build_util library")
endif()

find_package(chromium_base REQUIRED)
if(NOT TARGET ${base_LIB})
  message(FATAL_ERROR "not supported: using system provided chromium_base library")
endif()

# see https://doc.magnum.graphics/corrade/corrade-cmake.html#corrade-cmake-subproject
#find_package(Corrade REQUIRED PluginManager)

#if(NOT TARGET CONAN_PKG::corrade)
#  message(FATAL_ERROR "Use corrade from conan")
#endif()

#find_package( X11 REQUIRED )
#message(STATUS "X11_LIBRARIES = ${X11_LIBRARIES}")
#
#find_package( EXPAT REQUIRED )
#message(STATUS "EXPAT_LIBRARIES = ${EXPAT_LIBRARIES}")

#find_package( ZLIB REQUIRED )
#message(STATUS "ZLIB_LIBRARIES = ${ZLIB_LIBRARIES}")

#add_library( boost_outcome INTERFACE )
#target_include_directories( boost_outcome SYSTEM INTERFACE "submodules/boost.outcome/include" )
#get_target_property (BOOST_OUTCOME_IMPORTED_LOCATION boost_outcome INTERFACE_INCLUDE_DIRECTORIES)
#message( STATUS "boost_outcome=${BOOST_OUTCOME_IMPORTED_LOCATION}" )
#
#add_library(microsoft_gsl INTERFACE)
#target_include_directories(microsoft_gsl SYSTEM INTERFACE "submodules/GSL/include")
#get_target_property (microsoft_gsl_IMPORTED_LOCATION microsoft_gsl INTERFACE_INCLUDE_DIRECTORIES)
#message( STATUS "microsoft_gsl=${microsoft_gsl_IMPORTED_LOCATION}" )

message(STATUS "CMAKE_DL_LIBS = ${CMAKE_DL_LIBS}")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/)

set(flexnet_include_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/flexnet")
set(flexnet_src_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PlatformChecks.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommonOptions.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectFiles.cmake)

add_library(${LIB_NAME}-test-includes INTERFACE)

# $<INSTALL_INTERFACE:...> is exported using install(EXPORT)
# $<BUILD_INTERFACE:...> is exported using export(), or when the target is used by another target in the same buildsystem
macro(add_relative_include_dir TARGET VISIBILITY_BUILD VISIBILITY_INSTALL NEW_ELEM)
  target_include_directories(${TARGET}
    ${VISIBILITY_BUILD} "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${NEW_ELEM}>"
    ${VISIBILITY_INSTALL} "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}/${NEW_ELEM}>"
  )
  target_include_directories( ${LIB_NAME}-test-includes SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/${NEW_ELEM} )
endmacro(add_relative_include_dir)

message(STATUS "flexnet_SOURCES=${flexnet_SOURCES}")

add_library(${LIB_NAME} ${CORE_LIB_TYPE}
  ${flexnet_SOURCES}
)

#add_relative_include_dir(${LIB_NAME} PRIVATE PRIVATE "include/flexnet")

add_relative_include_dir(${LIB_NAME} PUBLIC PUBLIC "include")

list(APPEND USED_3DPARTY_LIBS
  ${base_LIB}
  ${build_util_LIB}
)

find_package(basis REQUIRED)
if(${basis_HEADER_DIR} STREQUAL "")
  message(FATAL_ERROR "unable to find basis_HEADER_DIR")
endif()
message(STATUS "basis_LIB=${basis_LIB}")

list(APPEND USED_3DPARTY_LIBS
  ${basis_LIB}
  #TODO: doctest
  #CONAN_PKG::doctest
  #CONAN_PKG::clang_folly_conan
  CONAN_PKG::boost
  CONAN_PKG::openssl
  CONAN_PKG::entt
  #CONAN_PKG::double-conversion
  #CONAN_PKG::glog
  #CONAN_PKG::lz4
  #CONAN_PKG::lzma
  #CONAN_PKG::zstd
  #CONAN_PKG::snappy
  #CONAN_PKG::libsodium
  #CONAN_PKG::libdwarf
  #CONAN_PKG::bzip2
  #CONAN_PKG::gflags
  #CONAN_PKG::libunwind
  #CONAN_PKG::libelf
  #CONAN_PKG::xz_utils
  #CONAN_PKG::corrade
  #Corrade::PluginManager
  ${USED_BOOST_LIBS}
)

set(USED_SYSTEM_LIBS
    Threads::Threads # pthread, https://cmake.org/cmake/help/v3.13/module/FindThreads.html
    ${X11_LIBRARIES} # https://cmake.org/cmake/help/v3.13/module/FindX11.html
    ${CMAKE_DL_LIBS} # https://cmake.org/cmake/help/v3.13/variable/CMAKE_DL_LIBS.html
    #EXPAT::EXPAT # https://cmake.org/cmake/help/v3.13/module/FindEXPAT.html
    #ZLIB::ZLIB # https://cmake.org/cmake/help/v3.13/module/FindZLIB.html
               # Custom libs
    stdc++fs # C++17 std::filesystem
    # @note: Order matters https://stackoverflow.com/a/10269201/10904212
    #${LIBIBERTY_LIBRARY} # used by folly
    #${DOUBLE_CONVERSION_LIBRARY} # used by folly
    #${LIBEVENT_LIB} # used by folly
    #${LZ4_LIBRARY} # used by folly
    #${LIBUNWIND_LIBRARIES} # used by folly
    #${LIBLZMA_LIBRARIES} # used by folly
    CACHE INTERNAL "USED_SYSTEM_LIBS")

target_link_libraries(${LIB_NAME} INTERFACE
  ${USED_3DPARTY_LIBS}
  ${USED_SYSTEM_LIBS}
)

set_property(TARGET ${LIB_NAME} PROPERTY CXX_STANDARD 17)

if(TARGET_EMSCRIPTEN)
  # use PROPERTY CXX_STANDARD 17
else()
  target_compile_features(${LIB_NAME}
    PUBLIC cxx_auto_type
    PRIVATE cxx_variadic_templates)
endif()

set(USED_SYSTEM_LIBS
    Threads::Threads # pthread, https://cmake.org/cmake/help/v3.13/module/FindThreads.html
    ${X11_LIBRARIES} # https://cmake.org/cmake/help/v3.13/module/FindX11.html
    ${CMAKE_DL_LIBS} # https://cmake.org/cmake/help/v3.13/variable/CMAKE_DL_LIBS.html
    #EXPAT::EXPAT # https://cmake.org/cmake/help/v3.13/module/FindEXPAT.html
    #ZLIB::ZLIB # https://cmake.org/cmake/help/v3.13/module/FindZLIB.html
               # Custom libs
    stdc++fs # C++17 std::filesystem
    # @note: Order matters https://stackoverflow.com/a/10269201/10904212
    #${LIBIBERTY_LIBRARY} # used by folly
    #${DOUBLE_CONVERSION_LIBRARY} # used by folly
    #${LIBEVENT_LIB} # used by folly
    #${LZ4_LIBRARY} # used by folly
    #${LIBUNWIND_LIBRARIES} # used by folly
    #${LIBLZMA_LIBRARIES} # used by folly
    CACHE INTERNAL "USED_SYSTEM_LIBS")

target_link_libraries(${LIB_NAME} PUBLIC#PRIVATE
  ${USED_3DPARTY_LIBS}
  ${USED_SYSTEM_LIBS}
)

set(DEBUG_LIBRARY_SUFFIX "-d")
set_target_properties(${LIB_NAME}
  PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" # TODO: /lib
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" # TODO: /lib
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" # TODO: /bin
    OUTPUT_NAME "${LIB_NAME}$<$<CONFIG:Debug>:${DEBUG_LIBRARY_SUFFIX}>"
    ENABLE_EXPORTS 1
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
    CMAKE_CXX_STANDARD_REQUIRED ON
)

# POSITION_INDEPENDENT_CODE for -fPIC
set_property(TARGET ${LIB_NAME}
  PROPERTY POSITION_INDEPENDENT_CODE ON)

target_compile_options(${LIB_NAME} PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wall>)

target_compile_options(${LIB_NAME} PRIVATE
  -frtti)

target_compile_options(${LIB_NAME} PRIVATE
  -fno-exceptions)

# TODO: DISABLE_DOCTEST
target_compile_definitions(${LIB_NAME} PUBLIC
  DISABLE_DOCTEST=1
)

target_compile_definitions(${LIB_NAME} PRIVATE
  ${flexnet_PRIVATE_DEFINES}
  # https://stackoverflow.com/a/30877725
  BOOST_SYSTEM_NO_DEPRECATED
  BOOST_ERROR_CODE_HEADER_ONLY
  BOOST_ASIO_STANDALONE=1
)

target_compile_definitions(${LIB_NAME} PUBLIC
  ${flexnet_PUBLIC_DEFINES}
)


# -------------------------------- Install -------------------------------------

# install and export steps
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Exports.cmake)

# It is always easier to navigate in an IDE when projects are organized in folders.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

## ---------------------------- Link Time Optimization -------------------------------- ##

if(ENABLE_LTO)
  # Check for LTO support (needs to be after project(...) )
  find_lto(CXX)

  # enable lto if available for non-debug configurations
  target_enable_lto(${LIB_NAME} optimized)
else(ENABLE_LTO)
  if(cmake_build_type_tolower MATCHES "release" )
    message(WARNING "Enable LTO in Release builds")
  endif()
endif(ENABLE_LTO)

## ---------------------------- warning level -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_compile_warnings.cmake)

## ---------------------------- valgrind -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_valgrind.cmake)

## ---------------------------- cppcheck -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_cppcheck.cmake)

## ---------------------------- clang_tidy -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_clang_tidy.cmake)

## ---------------------------- cppclean -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_cppclean.cmake)

## ---------------------------- oclint -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_oclint.cmake)

## ---------------------------- iwyu -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_iwyu.cmake)

## ---------------------------- clang_format -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_clang_format.cmake)

## ---------------------------- uncrustify -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_uncrustify.cmake)

## ---------------------------- sanitizers -------------------------------- ##

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_sanitizers.cmake)

## ---------------------------- gold linker -------------------------------- ##

# add_gold_linker
if(USE_LD_GOLD)
  add_gold_linker() # from cmake_helper_utils (conan package)
endif(USE_LD_GOLD)

## ---------------------------- ccache -------------------------------- ##

if(USE_CCACHE)
  add_ccache()
  target_ccache_summary(${LIB_NAME}) # from cmake_helper_utils (conan package)
endif(USE_CCACHE)

## ---------------------------- coverage -------------------------------- ##

if(USE_COVERAGE)
  add_coverage() # from cmake_helper_utils (conan package)
endif(USE_COVERAGE)

## ---------------------------- RULE_MESSAGES property -------------------------------- ##

# controls RULE_MESSAGES property
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_compile_log_level.cmake)

## ---------------------------- docs -------------------------------- ##

if(BUILD_DOXY_DOC)
  add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/docs )
endif()

## ---------------------------- tests -------------------------------- ##

if(ENABLE_TESTS)
  add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/tests )
endif()

# --------------------------------- Benchmark ----------------------------------

# TODO: https://github.com/jar-git/cmake-template/blob/b7fbb0d465fbbfd0a0e80977d9563f042314bbb3/example_app/CMakeLists.txt#L70
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/bench)

## ---------------------------- Uninstall -------------------------------- ##

if(ENABLE_UNINSTALL)
  # add `uninstall` cmake target
  addinstallManifest() # from cmake_helper_utils (conan package)
endif()

## ---------------------------- various files -------------------------------- ##

# Create a dummy target to hold various files in the project root (for IDE)
if (CONAN_AUTO_INSTALL)
  # NOTE: enable only when project is open in IDE
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/setup_misc_files.cmake)
endif (CONAN_AUTO_INSTALL)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples/server)
