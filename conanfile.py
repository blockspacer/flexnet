from conans import ConanFile, CMake, tools, AutoToolsBuildEnvironment, RunEnvironment, python_requires
from conans.errors import ConanInvalidConfiguration, ConanException
from conans.tools import os_info
import os, re, stat, fnmatch, platform, glob, traceback, shutil
from functools import total_ordering

# if you using python less than 3 use from distutils import strtobool
from distutils.util import strtobool

# conan runs the methods in this order:
# config_options(),
# configure(),
# requirements(),
# package_id(),
# build_requirements(),
# build_id(),
# system_requirements(),
# source(),
# imports(),
# build(),
# package(),
# package_info()

conan_build_helper = python_requires("conan_build_helper/[~=0.0]@conan/stable")

class flexnet_conan_project(conan_build_helper.CMakePackage):
    name = "flexnet"

    # Indicates License type of the packaged library
    # TODO (!!!)
    # license = "MIT"

    version = "master"

    # TODO (!!!)
    #url = "https://github.com/blockspacer/CXXCTP"

    description = "flexnet: C++ compile-time programming (serialization, reflection, code modification, enum to string, better enum, enum to json, extend or parse language, etc.)."
    topics = ('c++')

    options = {
        "shared": [True, False],
        "enable_ubsan": [True, False],
        "enable_asan": [True, False],
        "enable_msan": [True, False],
        "enable_tsan": [True, False],
        "enable_valgrind": [True, False]
    }

    default_options = (
        #"*:shared=False",
        "shared=False",
        "enable_ubsan=False",
        "enable_asan=False",
        "enable_msan=False",
        "enable_tsan=False",
        "enable_valgrind=False",
        # boost
        "boost:no_rtti=False",
        "boost:no_exceptions=True",
        "boost:without_python=True",
        "boost:without_coroutine=True",
        "boost:without_stacktrace=True",
        "boost:without_math=True",
        "boost:without_wave=True",
        "boost:without_contract=True",
        "boost:without_locale=True",
        "boost:without_random=True",
        "boost:without_regex=True",
        "boost:without_mpi=True",
        "boost:without_timer=True",
        "boost:without_thread=True",
        "boost:without_chrono=True",
        "boost:without_atomic=True",
        "boost:without_system=True",
        "boost:without_program_options=True",
        "boost:without_serialization=True",
        "boost:without_log=True",
        "boost:without_type_erasure=True",
        "boost:without_test=True",
        "boost:without_graph=True",
        "boost:without_graph_parallel=True",
        "boost:without_iostreams=True",
        "boost:without_context=True",
        "boost:without_fiber=True",
        "boost:without_filesystem=True",
        "boost:without_date_time=True",
        "boost:without_exception=True",
        "boost:without_container=True",
        # FakeIt
        "FakeIt:integration=catch",
        # openssl
        "openssl:shared=True",
        # chromium_base
        "chromium_base:use_alloc_shim=True",
        # chromium_tcmalloc
        "chromium_tcmalloc:use_alloc_shim=True",
    )

    # Custom attributes for Bincrafters recipe conventions
    _source_subfolder = "."
    _build_subfolder = "."

    # NOTE: no cmake_find_package due to custom FindXXX.cmake
    generators = "cmake", "cmake_paths", "virtualenv"

    # Packages the license for the conanfile.py
    #exports = ["LICENSE.md"]

    # If the source code is going to be in the same repo as the Conan recipe,
    # there is no need to define a `source` method. The source folder can be
    # defined like this
    exports_sources = ("LICENSE", "VERSION", "*.md", "include/*", "src/*",
                       "cmake/*", "examples/*", "CMakeLists.txt", "tests/*", "benchmarks/*",
                       "scripts/*", "tools/*", "codegen/*", "assets/*",
                       "docs/*", "licenses/*", "patches/*", "resources/*",
                       "submodules/*", "thirdparty/*", "third-party/*",
                       "third_party/*", "flexnet/*")

    settings = "os", "compiler", "build_type", "arch"

    # installs clang 10 from conan
    def _is_llvm_tools_enabled(self):
      return self._environ_option("ENABLE_LLVM_TOOLS", default = 'false')

    def _is_lwyu_enabled(self):
      return self._environ_option("ENABLE_LWYU", default = 'false')

    def _is_coverage_enabled(self):
      return self._environ_option("USE_COVERAGE", default = 'false')

    def _is_docs_enabled(self):
      return self._environ_option("BUILD_DOXY_DOC", default = 'false')

    def _is_benchmark_enabled(self):
      return self._environ_option("ENABLE_BENCHMARK", default = 'false')

    def _is_ccache_enabled(self):
      return self._environ_option("USE_CCACHE", default = 'false')

    def _is_cppcheck_enabled(self):
      return self._environ_option("ENABLE_CPPCHECK", default = 'false')

    def _is_clang_tidy_enabled(self):
      return self._environ_option("ENABLE_CLANG_TIDY", default = 'false')

    def _is_clang_format_enabled(self):
      return self._environ_option("ENABLE_CLANG_FORMAT", default = 'false')

    def _is_uncrustify_enabled(self):
      return self._environ_option("ENABLE_UNCRUSTIFY", default = 'false')

    def _is_iwyu_enabled(self):
      return self._environ_option("ENABLE_IWYU", default = 'false')

    def _is_cppclean_enabled(self):
      return self._environ_option("ENABLE_CPPCLEAN", default = 'false')

    def _is_lto_enabled(self):
      return self._environ_option("ENABLE_LTO", default = 'false')

    # sets cmake variables required to use clang 10 from conan
    def _is_compile_with_llvm_tools_enabled(self):
      return self._environ_option("COMPILE_WITH_LLVM_TOOLS", default = 'false')

    #def source(self):
    #  url = "https://github.com/....."
    #  self.run("git clone %s ......." % url)

    def configure(self):
        lower_build_type = str(self.settings.build_type).lower()

        if lower_build_type != "release" and not self._is_llvm_tools_enabled():
            self.output.warn('enable llvm_tools for Debug builds')

        if self._is_iwyu_enabled() and (not self._is_llvm_tools_enabled() or not self.options['llvm_tools'].include_what_you_use):
            raise ConanInvalidConfiguration("iwyu requires llvm_tools enabled and -o llvm_tools:include_what_you_use=True")

        if self._is_compile_with_llvm_tools_enabled() and not self._is_llvm_tools_enabled():
            raise ConanInvalidConfiguration("to compile with llvm_tools you must be enable llvm_tools")

        if self.options.enable_valgrind:
            self.options["flexnet"].enable_valgrind = True
            self.options["basis"].enable_valgrind = True
            self.options["chromium_base"].enable_valgrind = True

        if self.options.enable_ubsan \
           or self.options.enable_asan \
           or self.options.enable_msan \
           or self.options.enable_tsan:
            if not self._is_llvm_tools_enabled():
                raise ConanInvalidConfiguration("sanitizers require llvm_tools")

        if self.options.enable_ubsan \
           or self.options.enable_asan \
           or self.options.enable_msan \
           or self.options.enable_tsan:
            if not self.options["boost"].no_exceptions:
                raise ConanInvalidConfiguration("sanitizers require boost without exceptions")

        if self.options.enable_ubsan:
            self.options["flexnet"].enable_ubsan = True
            self.options["basis"].enable_ubsan = True
            self.options["chromium_base"].enable_ubsan = True
            self.options["corrade"].enable_ubsan = True
            if not self.options.use_system_boost:
              self.options["boost"].enable_ubsan = True
            if self._is_tests_enabled():
              self.options["conan_gtest"].enable_ubsan = True

        if self.options.enable_asan:
            self.options["flexnet"].enable_asan = True
            self.options["basis"].enable_asan = True
            self.options["chromium_base"].enable_asan = True
            self.options["corrade"].enable_asan = True
            if not self.options.use_system_boost:
              self.options["boost"].enable_asan = True
            if self._is_tests_enabled():
              self.options["conan_gtest"].enable_asan = True

        if self.options.enable_msan:
            self.options["flexnet"].enable_msan = True
            self.options["basis"].enable_msan = True
            self.options["chromium_base"].enable_msan = True
            self.options["corrade"].enable_msan = True
            if not self.options.use_system_boost:
              self.options["boost"].enable_msan = True
            if self._is_tests_enabled():
              self.options["conan_gtest"].enable_msan = True

        if self.options.enable_tsan:
            self.options["flexnet"].enable_tsan = True
            self.options["basis"].enable_tsan = True
            self.options["chromium_base"].enable_tsan = True
            self.options["corrade"].enable_tsan = True
            if not self.options.use_system_boost:
              self.options["boost"].enable_tsan = True
            if self._is_tests_enabled():
              self.options["conan_gtest"].enable_tsan = True

    def build_requirements(self):
        self.build_requires("cmake_platform_detection/master@conan/stable")
        self.build_requires("cmake_build_options/master@conan/stable")
        self.build_requires("cmake_helper_utils/master@conan/stable")

        if self.options.enable_tsan \
            or self.options.enable_msan \
            or self.options.enable_asan \
            or self.options.enable_ubsan:
          self.build_requires("cmake_sanitizers/master@conan/stable")

        if self._is_cppcheck_enabled():
          self.build_requires("cppcheck_installer/1.90@conan/stable")

        # provides clang-tidy, clang-format, IWYU, scan-build, etc.
        if self._is_llvm_tools_enabled():
          self.build_requires("llvm_tools/master@conan/stable")

    def requirements(self):

      if self._is_tests_enabled():
          self.requires("catch2/[>=2.1.0]@bincrafters/stable")
          self.requires("conan_gtest/release-1.10.0@conan/stable")
          self.requires("FakeIt/[>=2.0.4]@gasuketsu/stable")

      # TODO: support doctest
      #self.requires("doctest/[>=2.3.8]")

      self.requires("boost/1.71.0@dev/stable")

      self.requires("chromium_build_util/master@conan/stable")

      self.requires("chromium_base/master@conan/stable")

      self.requires("basis/master@conan/stable")

      self.requires("corrade/v2020.06@conan/stable")

      #self.requires("type_safe/0.2@conan/stable")

      #self.requires("double-conversion/3.1.1@bincrafters/stable")

      #self.requires("gflags/2.2.2@bincrafters/stable")

      #self.requires("glog/0.4.0@bincrafters/stable")

      # patched to support "openssl/OpenSSL_1_1_1-stable@conan/stable"
      #self.requires("libevent/2.1.11@dev/stable")

      # \note dispatcher must be thread-safe,
      # so use entt after patch https://github.com/skypjack/entt/issues/449
      # see https://github.com/skypjack/entt/commit/74f3df83dbc9fc4b43b8cfb9d71ba02234bd5c4a
      self.requires("entt/3.4.0")

      #self.requires("lz4/1.8.3@bincrafters/stable")

      # must match openssl version used in webrtc
      self.requires("openssl/OpenSSL_1_1_1-stable@conan/stable")

      #self.requires("OpenSSL/1.1.1c@conan/stable")

      # patched to support "openssl/OpenSSL_1_1_1-stable@conan/stable"
      #self.requires("zlib/v1.2.11@conan/stable")

      #self.requires("lzma/5.2.4@bincrafters/stable")

      #self.requires("zstd/1.3.8@bincrafters/stable")

      #self.requires("snappy/1.1.7@bincrafters/stable")

      #self.requires("bzip2/1.0.8@dev/stable")

      #self.requires("libsodium/1.0.18@bincrafters/stable")

      #self.requires("libelf/0.8.13@bincrafters/stable")

      #self.requires("libdwarf/20190505@bincrafters/stable")

      #self.requires("clang_folly_conan/v2019.01.14.00@conan/stable")

    def _configure_cmake(self):
        cmake = CMake(self)
        cmake.parallel = True
        cmake.verbose = True

        cmake.definitions["ENABLE_VALGRIND"] = 'ON'
        if not self.options.enable_valgrind:
            cmake.definitions["ENABLE_VALGRIND"] = 'OFF'

        cmake.definitions["ENABLE_UBSAN"] = 'ON'
        if not self.options.enable_ubsan:
            cmake.definitions["ENABLE_UBSAN"] = 'OFF'

        cmake.definitions["ENABLE_ASAN"] = 'ON'
        if not self.options.enable_asan:
            cmake.definitions["ENABLE_ASAN"] = 'OFF'

        cmake.definitions["ENABLE_MSAN"] = 'ON'
        if not self.options.enable_msan:
            cmake.definitions["ENABLE_MSAN"] = 'OFF'

        cmake.definitions["ENABLE_TSAN"] = 'ON'
        if not self.options.enable_tsan:
            cmake.definitions["ENABLE_TSAN"] = 'OFF'

        no_doctest = (str(self.settings.build_type).lower() != "debug"
          and str(self.settings.build_type).lower() != "relwithdebinfo")
        if no_doctest:
          cmake.definitions["DOCTEST_CONFIG_DISABLE"] = '1'
          self.output.info('Disabled DOCTEST')

        cmake.definitions["CONAN_AUTO_INSTALL"] = 'OFF'

        if self.options.shared:
            cmake.definitions["flexnet_BUILD_SHARED_LIBS"] = "ON"

        self.add_cmake_option(cmake, "ENABLE_TESTS", self._is_tests_enabled())

        self.add_cmake_option(cmake, "ENABLE_LWYU", self._is_lwyu_enabled())

        self.add_cmake_option(cmake, "USE_COVERAGE", self._is_coverage_enabled())

        self.add_cmake_option(cmake, "BUILD_DOXY_DOC", self._is_docs_enabled())

        self.add_cmake_option(cmake, "ENABLE_BENCHMARK", self._is_benchmark_enabled())

        self.add_cmake_option(cmake, "USE_CCACHE", self._is_ccache_enabled())

        self.add_cmake_option(cmake, "ENABLE_CPPCHECK", self._is_cppcheck_enabled())

        self.add_cmake_option(cmake, "ENABLE_CLANG_TIDY", self._is_clang_tidy_enabled())

        self.add_cmake_option(cmake, "ENABLE_CLANG_FORMAT", self._is_clang_format_enabled())

        self.add_cmake_option(cmake, "ENABLE_UNCRUSTIFY", self._is_uncrustify_enabled())

        self.add_cmake_option(cmake, "ENABLE_IWYU", self._is_iwyu_enabled())

        self.add_cmake_option(cmake, "ENABLE_CPPCLEAN", self._is_cppclean_enabled())

        self.add_cmake_option(cmake, "ENABLE_LTO", self._is_lto_enabled())

        self.add_cmake_option(cmake, "COMPILE_WITH_LLVM_TOOLS", self._is_compile_with_llvm_tools_enabled())


        cmake.configure(build_folder=self._build_subfolder)

        if self.settings.compiler == 'gcc':
            cmake.definitions["CMAKE_C_COMPILER"] = "gcc-{}".format(
                self.settings.compiler.version)
            cmake.definitions["CMAKE_CXX_COMPILER"] = "g++-{}".format(
                self.settings.compiler.version)

        cmake.definitions["CMAKE_TOOLCHAIN_FILE"] = 'conan_paths.cmake'

        return cmake

    def package(self):
        self.copy(pattern="LICENSE", dst="licenses", src=self._source_subfolder)
        cmake = self._configure_cmake()
        cmake.install()
        # Local build
        # see https://docs.conan.io/en/latest/developing_packages/editable_packages.html
        if not self.in_local_cache:
            self.copy("conanfile.py", dst=".", keep_path=False)

    def build(self):
        cmake = self._configure_cmake()
        if self.settings.compiler == 'gcc':
            cmake.definitions["CMAKE_C_COMPILER"] = "gcc-{}".format(
                self.settings.compiler.version)
            cmake.definitions["CMAKE_CXX_COMPILER"] = "g++-{}".format(
                self.settings.compiler.version)

        #cmake.definitions["CMAKE_TOOLCHAIN_FILE"] = 'conan_paths.cmake'

        # The CMakeLists.txt file must be in `source_folder`
        cmake.configure(source_folder=".")

        cpu_count = tools.cpu_count()
        self.output.info('Detected %s CPUs' % (cpu_count))

        # -j flag for parallel builds
        cmake.build(args=["--", "-j%s" % cpu_count])

        if self._is_tests_enabled():
          self.output.info('Running tests')
          cmake.build(args=["--target", "flexnet_run_all_tests", "--", "-j%s" % cpu_count])
          #self.run('ctest --parallel %s' % (cpu_count))
          # TODO: use cmake.test()

    # Importing files copies files from the local store to your project.
    def imports(self):
        dest = os.getenv("CONAN_IMPORT_PATH", "bin")
        self.copy("license*", dst=dest, ignore_case=True)
        self.copy("*.dll", dst=dest, src="bin")
        self.copy("*.so*", dst=dest, src="bin")
        self.copy("*.pdb", dst=dest, src="lib")
        self.copy("*.dylib*", dst=dest, src="lib")
        self.copy("*.lib*", dst=dest, src="lib")
        self.copy("*.a*", dst=dest, src="lib")

    # package_info() method specifies the list of
    # the necessary libraries, defines and flags
    # for different build configurations for the consumers of the package.
    # This is necessary as there is no possible way to extract this information
    # from the CMake install automatically.
    # For instance, you need to specify the lib directories, etc.
    def package_info(self):
        #self.cpp_info.libs = ["flexnet"]

        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libs = tools.collect_libs(self)
        self.cpp_info.libdirs = ["lib"]
        self.cpp_info.bindirs = ["bin"]
        self.env_info.LD_LIBRARY_PATH.append(
            os.path.join(self.package_folder, "lib"))
        self.env_info.PATH.append(os.path.join(self.package_folder, "bin"))
        for libpath in self.deps_cpp_info.lib_paths:
            self.env_info.LD_LIBRARY_PATH.append(libpath)

        #self.cpp_info.includedirs.append(os.getcwd())
        #self.cpp_info.includedirs.append(
        #  os.path.join("base", "third_party", "flexnet"))
        #self.cpp_info.includedirs.append(
        #  os.path.join("base", "third_party", "flexnet", "compat"))

        #if self.settings.os == "Linux":
        #  self.cpp_info.defines.append('HAVE_CONFIG_H=1')

        # in linux we need to link also with these libs
        #if self.settings.os == "Linux":
        #    self.cpp_info.libs.extend(["pthread", "dl", "rt"])

        #self.cpp_info.libs = tools.collect_libs(self)
        #self.cpp_info.defines.append('PDFLIB_DLL')
