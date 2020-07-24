# About

C++ lib

## Before installation: Add conan remotes

To be able to add the list of dependency remotes please type the following command:

```bash
cmake -E time conan config install conan/remotes/
# OR:
# cmake -E time conan config install conan/remotes_disabled_ssl/
```

## Installation

```bash
export CXX=clang++-6.0
export CC=clang-6.0

# NOTE: change `build_type=Debug` to `build_type=Release` in production
# NOTE: use --build=missing if you got error `ERROR: Missing prebuilt package`
CONAN_REVISIONS_ENABLED=1 \
CONAN_VERBOSE_TRACEBACK=1 \
CONAN_PRINT_RUN_COMMANDS=1 \
CONAN_LOGGING_LEVEL=10 \
GIT_SSL_NO_VERIFY=true \
    cmake -E time \
      conan create . conan/stable \
      -s build_type=Debug \
      -s llvm_tools:build_type=Release \
      --profile clang \
          -o flexnet:shared=False \
          -e flexnet:enable_tests=True
```

## How to increase log level

Use `--vmodule`.

```bash
./bin/Debug/example_server/example_server --vmodule=*main*=100,*=200 --enable-logging=stderr --log-level=100 --show-fps-counter --enable-features=console_terminal,remote_console --start_tracing --tracing_categories=*,disabled-by-default-memory-infra
```

## HOW TO BUILD WITH SANITIZERS ENABLED

Use `enable_asan` or `enable_ubsan`, etc.

```bash
# NOTE: change `build_type=Debug` to `build_type=Release` in production
CONAN_REVISIONS_ENABLED=1 \
    CONAN_VERBOSE_TRACEBACK=1 \
    CONAN_PRINT_RUN_COMMANDS=1 \
    CONAN_LOGGING_LEVEL=10 \
    GIT_SSL_NO_VERIFY=true \
    conan create . \
        conan/stable \
        -s build_type=Debug \
        -s llvm_tools:build_type=Release \
        --profile clang \
        --build missing \
        -s llvm_tools:build_type=Release \
        -e chromium_base:enable_tests=True \
        -o chromium_base:enable_tsan=True \
        -e chromium_base:enable_llvm_tools=True \
        -o chromium_base:use_alloc_shim=False \
        -e basis:enable_tests=True \
        -o basis:enable_tsan=True \
        -e basis:enable_llvm_tools=True \
        -e flexnet:enable_tests=True \
        -o flexnet:enable_tsan=True \
        -e flexnet:enable_llvm_tools=True \
        -o flexnet:shared=False \
        -o chromium_tcmalloc:use_alloc_shim=False \
        -o openssl:shared=True
```

## For contibutors: conan editable mode

With the editable packages, you can tell Conan where to find the headers and the artifacts ready for consumption in your local working directory.
There is no need to run `conan create` or `conan export-pkg`.

See for details [https://docs.conan.io/en/latest/developing_packages/editable_packages.html](https://docs.conan.io/en/latest/developing_packages/editable_packages.html)

Build locally:

```bash
CONAN_REVISIONS_ENABLED=1 \
CONAN_VERBOSE_TRACEBACK=1 \
CONAN_PRINT_RUN_COMMANDS=1 \
CONAN_LOGGING_LEVEL=10 \
GIT_SSL_NO_VERIFY=true \
  cmake -E time \
    conan install . \
    --install-folder local_build \
    -s build_type=Debug \
    -s llvm_tools:build_type=Release \
    --profile clang \
      -o flexnet:shared=False \
      -e flexnet:enable_tests=True

CONAN_REVISIONS_ENABLED=1 \
CONAN_VERBOSE_TRACEBACK=1 \
CONAN_PRINT_RUN_COMMANDS=1 \
CONAN_LOGGING_LEVEL=10 \
GIT_SSL_NO_VERIFY=true \
  cmake -E time \
    conan source . --source-folder local_build

conan build . \
  --build-folder local_build

conan package . \
  --build-folder local_build \
  --package-folder local_build/package_dir
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
  --package-folder local_build/package_dir
```

Build your test project

In order to revert the editable mode just remove the link using:

```bash
conan editable remove \
  flexnet/master@conan/stable
```

## For contibutors: IWYU

Make sure you use `Debug` build with `-e flexnet:enable_llvm_tools=True`

include-what-you-use (IWYU) is a project intended to optimise includes.

It will calculate the required headers and add and remove includes as appropriate.

See for details [https://include-what-you-use.org/](https://include-what-you-use.org/)

Usage (runs cmake with `-DENABLE_IWYU=ON`):

```bash
export CXX=clang++-6.0
export CC=clang-6.0

# creates local build in separate folder and runs cmake targets
cmake -DIWYU="ON" -DCLEAN_OLD="ON" -P tools/run_tool.cmake
```