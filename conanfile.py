from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class LibAutoUpdaterConan(ConanFile):
    name = "libautoupdater"
    version = "0.1.5"
    license = "MIT"
    url = "https://github.com/sorrowfeng/libAutoUpdater"
    homepage = "https://github.com/sorrowfeng/libAutoUpdater"
    description = "C++17 cross-platform desktop online update library."
    topics = ("updater", "desktop", "cmake", "cpp17")
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_curl": [True, False],
        "with_openssl": [True, False],
        "build_updater": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_curl": True,
        "with_openssl": True,
        "build_updater": True,
    }
    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "updater/*",
        "LICENSE",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def requirements(self):
        if self.options.with_curl:
            self.requires("libcurl/[>=8.0 <9]")
        if self.options.with_openssl:
            self.requires("openssl/[>=3.0 <4]")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.variables["LIBAUTOUPDATER_BUILD_EXAMPLES"] = False
        tc.variables["LIBAUTOUPDATER_BUILD_TESTS"] = False
        tc.variables["LIBAUTOUPDATER_BUILD_UPDATER"] = bool(self.options.build_updater)
        tc.variables["LIBAUTOUPDATER_WITH_CURL"] = bool(self.options.with_curl)
        tc.variables["LIBAUTOUPDATER_WITH_OPENSSL"] = bool(self.options.with_openssl)
        tc.variables["LIBAUTOUPDATER_WITH_QT"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "libAutoUpdater")
        self.cpp_info.set_property("cmake_target_name", "libAutoUpdater::libAutoUpdater")
        self.cpp_info.libs = ["libAutoUpdater"]
        # Native network backends link system libraries that must be visible
        # to consumers of a static package.
        if not self.options.with_curl:
            if self.settings.os == "Windows":
                self.cpp_info.system_libs.append("winhttp")
            elif self.settings.os in ("Macos", "iOS", "tvOS", "watchOS"):
                self.cpp_info.frameworks.extend(["CFNetwork", "CoreFoundation"])
