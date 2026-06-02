class Libautoupdater < Formula
  desc "C++17 cross-platform desktop online update library"
  homepage "https://github.com/sorrowfeng/libAutoUpdater"
  url "https://github.com/sorrowfeng/libAutoUpdater/archive/refs/tags/v0.1.2.tar.gz"
  sha256 "REPLACE_WITH_RELEASE_TARBALL_SHA256"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "curl"
  depends_on "openssl@3"

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DLIBAUTOUPDATER_BUILD_EXAMPLES=OFF",
           "-DLIBAUTOUPDATER_BUILD_TESTS=OFF",
           "-DLIBAUTOUPDATER_BUILD_UPDATER=ON",
           "-DLIBAUTOUPDATER_WITH_QT=OFF",
           *std_cmake_args
    system "cmake", "--build", "build", "--parallel"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"main.cpp").write <<~CPP
      #include <libAutoUpdater/Version.h>
      int main() {
          return autoupdater::libraryVersion().major() >= 0 ? 0 : 1;
      }
    CPP
    system ENV.cxx, "main.cpp", "-std=c++17", "-I#{include}", "-L#{lib}", "-llibAutoUpdater", "-o", "probe"
    system "./probe"
  end
end
