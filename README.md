# About

C++ lib

## Before installation: Add conan remotes

To be able to add the list of dependency remotes please type the following command:

```bash
cmake -E time conan config install conan/remotes/
# OR:
# cmake -E time conan config install conan/remotes_disabled_ssl/
```

## Before installation

- [Installation Guide](https://blockspacer.github.io/flex_docs/download/)

- conan packages

NOTE: cling with LLVM build may take couple of hours.

Command below uses `--profile clang`.

Example conan profile `~/.conan/profiles/clang`:

```txt
[settings]
# We are building in Ubuntu Linux
os_build=Linux
os=Linux
arch_build=x86_64
arch=x86_64

compiler=clang
compiler.version=10
compiler.libcxx=libstdc++11

[env]
CC=/usr/bin/clang-10
CXX=/usr/bin/clang++-10

[build_requires]
cmake_installer/3.15.5@conan/stable
```

Before creation of conan profile file, see: https://docs.conan.io/en/latest/using_packages/using_profiles.html.

We use `buildConanThirdparty.cmake` script to download and install conan packages.

```bash
# NOTE: don't forget to re-run `conan install` or `conan create` after command below
# NOTE: change `build_type=Debug` to `build_type=Release` in production
cmake \
  -DEXTRA_CONAN_OPTS=\
"--profile;clang\
;-s;build_type=Debug\
;-s;cling_conan:build_type=Release\
;-s;llvm_tools:build_type=Release\
;--build;missing" \
  -DENABLE_LLVM_TOOLS=FALSE \
  -DENABLE_CLING=TRUE \
  -P tools/buildConanThirdparty.cmake

# clean build cache
conan remove "*" --build --force
```

NOTE: set `-DENABLE_CLING=FALSE` if you already installed Cling using `tools/buildConanThirdparty.cmake` above.

- llvm_tools package

NOTE: `llvm_tools` package is optional; you can skip it using `enable_llvm_tools=False` like so: `-e flextool:enable_llvm_tools=False -e basis:enable_llvm_tools=False -e chromium_base:enable_llvm_tools=False`

NOTE: LLVM build may take couple of hours.

NOTE: `-DENABLE_LLVM_TOOLS=TRUE` does the same (using `buildConanThirdparty.cmake`)

Command below uses `--profile clang`. Before creation of conan profile file, see: https://docs.conan.io/en/latest/using_packages/using_profiles.html.

You can install `llvm_tools` like so:

```bash
git clone https://github.com/blockspacer/llvm_tools.git
cd llvm_tools
conan create . \
  conan/stable \
  -s build_type=Release \
  --profile clang \
  --build missing

# clean build cache
conan remove "*" --build --force
```

Up-to-date instructions are found here: [https://github.com/blockspacer/llvm_tools](https://github.com/blockspacer/llvm_tools)

## Installation

```bash
export VERBOSE=1
export CONAN_REVISIONS_ENABLED=1
export CONAN_VERBOSE_TRACEBACK=1
export CONAN_PRINT_RUN_COMMANDS=1
export CONAN_LOGGING_LEVEL=10
export GIT_SSL_NO_VERIFY=true

export CXX=clang++-10
export CC=clang-10

# NOTE: change `build_type=Debug` to `build_type=Release` in production
# NOTE: use --build=missing if you got error `ERROR: Missing prebuilt package`
cmake -E time \
  conan create . conan/stable \
  -s build_type=Debug \
  -s llvm_tools:build_type=Release \
  --profile clang \
      -o chromium_base:use_alloc_shim=True \
      -o chromium_tcmalloc:use_alloc_shim=True \
      -o flexnet:shared=False \
      -e flexnet:enable_tests=True

# clean build cache
conan remove "*" --build --force
```

## How to increase log level

Use `--vmodule`.

```bash
./bin/Debug/server_executable/server_executable \
  --vmodule=*main*=100,*=200 \
  --enable-logging=stderr \
  --log-level=100 \
  --enable-features=console_terminal,remote_console \
  --start_tracing \
  --tracing_categories=*,disabled-by-default-memory-infra \
  --log_file_conf_server_executable="/tmp/logme.log"
```

NOTE: if you want verbose error reporting, than
you can add `,print_status_macro_stack_trace,print_status_macro_error`
into `--enable-features=`

NOTE: if you want to run app under debugger, than
you can prepend `gdb -ex r -ex bt -ex q --args`

## HOW TO BUILD WITH SANITIZERS ENABLED

Use `enable_asan` or `enable_ubsan`, etc.

```bash
# NOTE: also re-build all deps with sanitizers enabled

mkdir build_tsan

cd build_tsan

export TSAN_OPTIONS="handle_segv=0:disable_coredump=0:abort_on_error=1:report_thread_leaks=0"

# make sure that env. var. TSAN_SYMBOLIZER_PATH points to llvm-symbolizer
# conan package llvm_tools provides llvm-symbolizer
# and prints its path during cmake configure step
# echo $TSAN_SYMBOLIZER_PATH
export TSAN_SYMBOLIZER_PATH=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/llvm-symbolizer" | head -n 1)

export CC=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/clang" | head -n 1)

export CXX=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/clang++" | head -n 1)

# must exist
file $(dirname $CXX)/../lib/clang/10.0.1/lib/linux/libclang_rt.tsan_cxx-x86_64.a

export CFLAGS="-fsanitize=thread -fuse-ld=lld -stdlib=libc++ -lc++ -lc++abi -lunwind"

export CXXFLAGS="-fsanitize=thread -fuse-ld=lld -stdlib=libc++ -lc++ -lc++abi -lunwind"

export LDFLAGS="-stdlib=libc++ -lc++ -lc++abi -lunwind"

export VERBOSE=1
export CONAN_REVISIONS_ENABLED=1
export CONAN_VERBOSE_TRACEBACK=1
export CONAN_PRINT_RUN_COMMANDS=1
export CONAN_LOGGING_LEVEL=10
export GIT_SSL_NO_VERIFY=true

# NOTE: NO `--profile` argument cause we use `CXX` env. var
# NOTE: change `build_type=Debug` to `build_type=Release` in production
conan install .. \
    conan/stable \
    -s build_type=Debug \
    -s llvm_tools:build_type=Release \
    --build chromium_base \
    --build basis \
    --build flexnet \
    --build missing \
    --build cascade \
    -s llvm_tools:build_type=Release \
    -o llvm_tools:enable_tsan=True \
    -o llvm_tools:include_what_you_use=False \
    -s llvm_tools:compiler=clang \
    -s llvm_tools:compiler.version=10 \
    -s llvm_tools:compiler.libcxx=libstdc++11 \
    -o perfetto:is_hermetic_clang=False \
    -o perfetto:is_tsan=True \
    -e abseil:enable_llvm_tools=True \
    -e chromium_base:enable_tests=True \
    -o chromium_base:enable_tsan=True \
    -e chromium_base:enable_llvm_tools=True \
    -o chromium_base:use_alloc_shim=False \
    -e basis:enable_tests=True \
    -o basis:enable_tsan=True \
    -e basis:enable_llvm_tools=True \
    -e flexnet:compile_with_llvm_tools=True \
    -e chromium_base:compile_with_llvm_tools=True \
    -e basis:compile_with_llvm_tools=True \
    -e boost:enable_llvm_tools=True \
    -o boost:enable_tsan=True \
    -o boost:no_exceptions=True \
    -e boost:compile_with_llvm_tools=True \
    -s compiler=clang \
    -s compiler.version=10 \
    -s compiler.libcxx=libc++ \
    -e flexnet:enable_tests=True \
    -o flexnet:enable_tsan=True \
    -e flexnet:enable_llvm_tools=True \
    -o flexnet:shared=False \
    -o chromium_tcmalloc:use_alloc_shim=False \
    -o openssl:shared=True \
    -o openssl:enable_tsan=True \
    -e openssl:enable_llvm_tools=True \
    -e openssl:compile_with_llvm_tools=True \
    -e conan_gtest:compile_with_llvm_tools=True \
    -e conan_gtest:enable_llvm_tools=True \
    -e benchmark:compile_with_llvm_tools=True \
    -e benchmark:enable_llvm_tools=True \
    -e fmt:compile_with_llvm_tools=True \
    -e fmt:enable_llvm_tools=True \
    -e chromium_libxml:compile_with_llvm_tools=True \
    -e chromium_libxml:enable_llvm_tools=True \
    -e chromium_icu:compile_with_llvm_tools=True \
    -e chromium_icu:enable_llvm_tools=True \
    -e chromium_zlib:compile_with_llvm_tools=True \
    -e chromium_zlib:enable_llvm_tools=True \
    -e chromium_libevent:compile_with_llvm_tools=True \
    -e chromium_libevent:enable_llvm_tools=True \
    -e chromium_xdg_user_dirs:compile_with_llvm_tools=True \
    -e chromium_xdg_user_dirs:enable_llvm_tools=True \
    -e chromium_xdg_mime:compile_with_llvm_tools=True \
    -e chromium_xdg_mime:enable_llvm_tools=True \
    -e chromium_dynamic_annotations:compile_with_llvm_tools=True \
    -e chromium_dynamic_annotations:enable_llvm_tools=True \
    -e chromium_modp_b64:compile_with_llvm_tools=True \
    -e chromium_modp_b64:enable_llvm_tools=True \
    -e chromium_compact_enc_det:compile_with_llvm_tools=True \
    -e chromium_compact_enc_det:enable_llvm_tools=True \
    -e corrade:compile_with_llvm_tools=True \
    -e corrade:enable_llvm_tools=True

# NOTE: change `build_type=Debug` to `build_type=Release` in production
export build_type=Debug

# optional
# remove old CMakeCache
(rm CMakeCache.txt || true)

(rm -rf generated || true)

# NOTE: -DENABLE_TSAN=ON
cmake -E time cmake .. \
  -DCMAKE_VERBOSE_MAKEFILE=TRUE \
  -DENABLE_TSAN=ON \
  -DENABLE_TESTS=TRUE \
  -DBASE_NEED_GEN_BUILD_DATE=FALSE \
  -DBUILD_SHARED_LIBS=FALSE \
  -DCONAN_AUTO_INSTALL=OFF \
  -DCMAKE_BUILD_TYPE=${build_type} \
  -DCOMPILE_WITH_LLVM_TOOLS=TRUE

# remove old build artifacts
rm -rf bin
find . -iname '*.o' -exec rm {} \;
find . -iname '*.a' -exec rm {} \;
find . -iname '*.dll' -exec rm {} \;
find . -iname '*.lib' -exec rm {} \;

# build code
cmake -E time cmake --build . \
  --config ${build_type} \
  -- -j8

# run unit tests for flexnet
cmake -E time cmake --build . \
  --config ${build_type} \
  --target flexnet_run_all_tests

# do not forget to reset compile flags
unset CFLAGS
unset CXXFLAGS
unset LDFLAGS
```

## For contibutors: conan editable mode

With the editable packages, you can tell Conan where to find the headers and the artifacts ready for consumption in your local working directory.
There is no need to run `conan create` or `conan export-pkg`.

See for details [https://docs.conan.io/en/latest/developing_packages/editable_packages.html](https://docs.conan.io/en/latest/developing_packages/editable_packages.html)

Build locally:

```bash
export VERBOSE=1
export CONAN_REVISIONS_ENABLED=1
export CONAN_VERBOSE_TRACEBACK=1
export CONAN_PRINT_RUN_COMMANDS=1
export CONAN_LOGGING_LEVEL=10
export GIT_SSL_NO_VERIFY=true

cmake -E time \
  conan install . \
  --install-folder local_build \
  -s build_type=Debug \
  -s llvm_tools:build_type=Release \
  --profile clang \
    -o flexnet:shared=False \
    -e flexnet:enable_tests=True

cmake -E time \
  conan source . \
  --source-folder local_build \
  --install-folder local_build

conan build . \
  --build-folder local_build

conan package . \
  --build-folder local_build \
  --package-folder local_build/package_dir \
  --source-folder local_build \
  --install-folder local_build
```

Set package to editable mode:

```bash
conan editable add local_build/package_dir \
  flexnet/master@conan/stable
```

Note that `conanfile.py` modified to detect local builds via `self.in_local_cache`

After change source in folder local_build (run commands in source package folder):

```
conan build . \
  --build-folder local_build

conan package . \
  --build-folder local_build \
  --package-folder local_build/package_dir \
  --source-folder local_build \
  --install-folder local_build
```

Build your test project

In order to revert the editable mode just remove the link using:

```bash
conan editable remove \
  flexnet/master@conan/stable
```

## For contibutors: conan workspace with sanitizers

Allows to build multiple projects at once, it just creates `CMakeLists.txt` with `add_subdirectory` pointing to each package folder.

NOTE: You can open workspace in IDE as usual CMake based project (change build directory to WorkspaceProject path)!

See for details [https://docs.conan.io/en/latest/developing_packages/workspaces.html](https://docs.conan.io/en/latest/developing_packages/workspaces.html)

For example, we want to build both flexnet and basis at the same time (flexnet requires basis).

```bash
# change ~ to desired build folder
cd ~

# Replace paths to yours!
# Make sure each project in NOT in editable mode!
cat <<EOF > ~/flexnetws.yml
editables:
    chromium_base/master@conan/stable:
        path: /home/..../chromium_base_conan
    basis/master@conan/stable:
        path: /home/..../basis
    flexnet/master@conan/stable:
        path: /home/..../flexnet
layout: layout_flex
workspace_generator: cmake
root:
    - chromium_base/master@conan/stable
    - basis/master@conan/stable
    - flexnet/master@conan/stable
EOF

cat <<EOF > ~/layout_flex
# This helps to define the location of CMakeLists.txt within package
[source_folder]
.

# This defines where the conanbuildinfo.cmake will be written to
[build_folder]
build/{{settings.build_type}}
EOF
```

```bash
export CC=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/clang" | head -n 1)

export CXX=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/clang++" | head -n 1)

# must exist
file $(dirname $CXX)/../lib/clang/10.0.1/lib/linux/libclang_rt.tsan_cxx-x86_64.a

# see https://github.com/google/sanitizers/wiki/SanitizerCommonFlags
export MSAN_OPTIONS="poison_in_dtor=1:fast_unwind_on_malloc=0:check_initialization_order=true:handle_segv=0:detect_leaks=1:detect_stack_use_after_return=1:disable_coredump=0:abort_on_error=1"
# you can also set LSAN_OPTIONS=suppressions=suppr.txt

# make sure that env. var. MSAN_SYMBOLIZER_PATH points to llvm-symbolizer
# conan package llvm_tools provides llvm-symbolizer
# and prints its path during cmake configure step
# echo $MSAN_SYMBOLIZER_PATH
export MSAN_SYMBOLIZER_PATH=$(find ~/.conan/data/llvm_tools/master/conan/stable/package/ -path "*bin/llvm-symbolizer" | head -n 1)

export CFLAGS="-fsanitize=memory -fuse-ld=lld -stdlib=libc++ -lc++ -lc++abi -lunwind"

export CXXFLAGS="-fsanitize=memory -fuse-ld=lld -stdlib=libc++ -lc++ -lc++abi -lunwind"

export LDFLAGS="-stdlib=libc++ -lc++ -lc++abi -lunwind"

mkdir build_flexnet

cd build_flexnet

cat <<EOF > CMakeLists.txt
cmake_minimum_required(VERSION 3.0)

project(WorkspaceProject)

include(\${CMAKE_BINARY_DIR}/conanworkspace.cmake)

list(PREPEND CMAKE_MODULE_PATH "\${PACKAGE_chromium_base_SRC}/cmake")
list(PREPEND CMAKE_MODULE_PATH "\${PACKAGE_basis_SRC}/cmake")
list(PREPEND CMAKE_MODULE_PATH "\${PACKAGE_flexnet_SRC}/cmake")

conan_workspace_subdirectories()

add_dependencies(basis chromium_base-static)

add_dependencies(flexnet basis)
EOF

# must contain `include(${CMAKE_BINARY_DIR}/conanworkspace.cmake)` without slash `\` (slash added for bash cat command)
cat CMakeLists.txt

# combines options from all projects
conan workspace install \
  ../flexnetws.yml \
        -s build_type=Debug \
        -s llvm_tools:build_type=Release \
        --build chromium_base \
        --build basis \
        --build missing \
        --build cascade \
        -s llvm_tools:build_type=Release \
        -o llvm_tools:include_what_you_use=False \
        -o llvm_tools:enable_msan=True \
        -s llvm_tools:compiler=clang \
        -s llvm_tools:compiler.version=10 \
        -s llvm_tools:compiler.libcxx=libstdc++11 \
        -o perfetto:is_hermetic_clang=False \
        -o perfetto:is_msan=True \
        -e abseil:enable_llvm_tools=True \
        -e chromium_base:enable_tests=True \
        -e chromium_base:enable_llvm_tools=True \
        -o chromium_base:use_alloc_shim=False \
        -o chromium_base:enable_msan=True \
        -e chromium_base:compile_with_llvm_tools=True \
        -e basis:enable_tests=True \
        -e basis:enable_llvm_tools=True \
        -o basis:enable_msan=True \
        -e basis:compile_with_llvm_tools=True \
        -e flexnet:compile_with_llvm_tools=True \
        -o flexnet:enable_msan=True \
        -e flexnet:enable_tests=True \
        -e flexnet:enable_llvm_tools=True \
        -o flexnet:shared=False \
        -s compiler=clang \
        -s compiler.version=10 \
        -s compiler.libcxx=libc++ \
        -o chromium_tcmalloc:use_alloc_shim=False \
        -e boost:enable_llvm_tools=True \
        -o boost:enable_msan=True \
        -e boost:compile_with_llvm_tools=True \
        -o boost:no_exceptions=True \
        -o openssl:shared=True \
        -o openssl:enable_msan=True \
        -e openssl:enable_llvm_tools=True \
        -e openssl:compile_with_llvm_tools=True \
        -e conan_gtest:compile_with_llvm_tools=True \
        -e conan_gtest:enable_llvm_tools=True \
        -e benchmark:compile_with_llvm_tools=True \
        -e benchmark:enable_llvm_tools=True \
        -e fmt:compile_with_llvm_tools=True \
        -e fmt:enable_llvm_tools=True \
        -e chromium_libxml:compile_with_llvm_tools=True \
        -e chromium_libxml:enable_llvm_tools=True \
        -e chromium_icu:compile_with_llvm_tools=True \
        -e chromium_icu:enable_llvm_tools=True \
        -e chromium_zlib:compile_with_llvm_tools=True \
        -e chromium_zlib:enable_llvm_tools=True \
        -e chromium_libevent:compile_with_llvm_tools=True \
        -e chromium_libevent:enable_llvm_tools=True \
        -e chromium_xdg_user_dirs:compile_with_llvm_tools=True \
        -e chromium_xdg_user_dirs:enable_llvm_tools=True \
        -e chromium_xdg_mime:compile_with_llvm_tools=True \
        -e chromium_xdg_mime:enable_llvm_tools=True \
        -e chromium_dynamic_annotations:compile_with_llvm_tools=True \
        -e chromium_dynamic_annotations:enable_llvm_tools=True \
        -e chromium_modp_b64:compile_with_llvm_tools=True \
        -e chromium_modp_b64:enable_llvm_tools=True \
        -e chromium_compact_enc_det:compile_with_llvm_tools=True \
        -e chromium_compact_enc_det:enable_llvm_tools=True \
        -e corrade:compile_with_llvm_tools=True \
        -e corrade:enable_llvm_tools=True
```

Build into folder created by `conan workspace install`:

```bash
# NOTE: change `build_type=Debug` to `build_type=Release` in production
export build_type=Debug

# optional
# remove old CMakeCache
(rm CMakeCache.txt || true)
(rm -rf generated || true)

# NOTE: -DENABLE_MSAN=ON
cmake -E time cmake . \
  -DCMAKE_VERBOSE_MAKEFILE=TRUE \
  -DENABLE_MSAN=ON \
  -DENABLE_TESTS=TRUE \
  -DBASE_NEED_GEN_BUILD_DATE=FALSE \
  -DENABLE_DOCTEST=ON \
  -DBUILD_SHARED_LIBS=FALSE \
  -DCONAN_AUTO_INSTALL=OFF \
  -DCMAKE_BUILD_TYPE=${build_type} \
  -DCOMPILE_WITH_LLVM_TOOLS=TRUE \
  -DUSE_TEST_SUPPORT=TRUE \
  -DUSE_ALLOC_SHIM=FALSE

# remove generated files
# change paths to yours
# rm ~/flexnet/build/Debug/*generated*

# build code
cmake -E time cmake --build . \
  --config ${build_type} \
  -- -j8

# run unit tests for flexnet
cmake -E time cmake --build . \
  --config ${build_type} \
  --target flexnet_run_all_tests

# run unit tests for basis
cmake -E time cmake --build . \
  --config ${build_type} \
  --target basis_run_all_tests

# do not forget to reset compile flags
unset CFLAGS
unset CXXFLAGS
unset LDFLAGS
```

Workspace allows to make quick changes in existing source files.

We use `self.in_local_cache` to detect conan editable mode:

```python
# Local build
# see https://docs.conan.io/en/latest/developing_packages/editable_packages.html
if not self.in_local_cache:
    self.copy("conanfile.py", dst=".", keep_path=False)
```

Make sure that all targets have globally unique names.

For example, you can not have in each project target with same name like "test". You can solve that issue by adding project-specific prefix to name of each target like "${ROOT_PROJECT_NAME}-test_main_gtest".

Because `CMAKE_BINARY_DIR` will point to folder created by `conan workspace install` - make sure that you prefer `CMAKE_CURRENT_BINARY_DIR` to `CMAKE_BINARY_DIR` etc.

Example if you want to build code without sanitizers:

```bash
export CXX=clang++-10
export CC=clang-10

# NOTE: change `build_type=Debug` to `build_type=Release` in production
export build_type=Debug

# combines options from all projects
conan workspace install \
  ../flexnetws.yml \
        --profile=clang \
        -s compiler=clang \
        -s compiler.version=10 \
        -s llvm_tools:compiler.libcxx=libstdc++11 \
        -s build_type=${build_type} \
        -s llvm_tools:build_type=Release \
        -s llvm_tools:compiler=clang \
        -s llvm_tools:compiler.version=10 \
        -s llvm_tools:compiler.libcxx=libstdc++11 \
        --build missing \
        --build cascade \
        --build chromium_tcmalloc \
        --build conan_gtest \
        --build boost

# optional
# remove old CMakeCache
(rm CMakeCache.txt || true)

(rm -rf generated || true)

# NOTE: -DENABLE_TSAN=ON
cmake -E time cmake . \
  -DCMAKE_VERBOSE_MAKEFILE=TRUE \
  -DENABLE_TESTS=TRUE \
  -DBASE_NEED_GEN_BUILD_DATE=FALSE \
  -DENABLE_DOCTEST=ON \
  -DBUILD_SHARED_LIBS=FALSE \
  -DCONAN_AUTO_INSTALL=OFF \
  -DUSE_TEST_SUPPORT=TRUE \
  -DCMAKE_BUILD_TYPE=${build_type} \
  -DUSE_ALLOC_SHIM=TRUE

# remove old build artifacts
rm -rf bin
find . -iname '*.o' -exec rm {} \;
find . -iname '*.a' -exec rm {} \;
find . -iname '*.dll' -exec rm {} \;
find . -iname '*.lib' -exec rm {} \;

# build code
cmake -E time cmake --build . \
  --config ${build_type} \
  -- -j8

# run unit tests for flexnet
cmake -E time cmake --build . \
  --config ${build_type} \
  --target flexnet_run_all_tests

# run unit tests for basis
cmake -E time cmake --build . \
  --config ${build_type} \
  --target basis_run_all_tests
```

## For contibutors: IWYU

Make sure you use `Debug` build with `-e flexnet:enable_llvm_tools=True`

include-what-you-use (IWYU) is a project intended to optimise includes.

It will calculate the required headers and add and remove includes as appropriate.

See for details [https://include-what-you-use.org/](https://include-what-you-use.org/)

Usage (runs cmake with `-DENABLE_IWYU=ON`):

```bash
export CXX=clang++-10
export CC=clang-10

# creates local build in separate folder and runs cmake targets
cmake -DIWYU="ON" -DCLEAN_OLD="ON" -P tools/run_tool.cmake
```

## System configuration (Linux for high loads)

```bash
sysctl -w 'fs.nr_open=10000000'

sysctl -w  'net.core.rmem_max=12582912'
sysctl -w 'net.core.wmem_max=12582912'

sysctl -w 'net.ipv4.tcp_mem=10240 87380 12582912'
sysctl -w 'net.ipv4.tcp_rmem=10240 87380 12582912'
sysctl -w 'net.ipv4.tcp_wmem=10240 87380 12582912'
sysctl -w 'net.core.somaxconn=15000'
```

See:
* https://medium.com/@pawilon/tuning-your-linux-kernel-and-haproxy-instance-for-high-loads-1a2105ea553e
* https://gist.github.com/mustafaturan/47268d8ad6d56cadda357e4c438f51ca

## Disclaimer

That open source project based on the Google Chromium project.

This is not official Google product.

Portions Copyright (c) Google Inc.

See LICENSE files.
