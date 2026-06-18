#include "TestCommon.h"

#include "ApplyLauncher.h"
#include "libAutoUpdater/Config.h"
#include "libAutoUpdater/interfaces/IProcessLauncher.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::filesystem::path uniqueTempDir() {
    auto base = std::filesystem::temp_directory_path() / "libAutoUpdater-apply-launcher-test";
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = base / std::to_string(tick);
    std::filesystem::create_directories(path);
    return path;
}

class CapturingProcessLauncher final : public autoupdater::IProcessLauncher {
  public:
    autoupdater::Result<void> launch(const autoupdater::ProcessLaunchRequest& request) noexcept override {
        request_ = request;
        return autoupdater::Result<void>::ok();
    }

    std::optional<autoupdater::ProcessLaunchRequest> request_;
};

} // namespace

void testApplyLauncherUsesStagedUpdaterCopy() {
    const auto root = uniqueTempDir();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto installDir = root / "install";
    const auto tempDir = root / "staging";
    const auto updaterExe = installDir / "autoupdater_apply.exe";
    const auto planPath = tempDir / "apply-plan.json";
    writeFile(updaterExe, "updater-binary");
    writeFile(planPath, "{}");

    autoupdater::Config config;
    config.installDir = installDir;
    config.tempDir = tempDir;
    config.updaterExecutable = updaterExe;

    CapturingProcessLauncher launcher;
    auto launched = autoupdater::launchApplyProcess(config, planPath, launcher);
    LAU_REQUIRE(launched);
    LAU_REQUIRE(launcher.request_.has_value());
    LAU_REQUIRE(launcher.request_->workingDirectory == installDir);
    LAU_REQUIRE(launcher.request_->detached);
    LAU_REQUIRE(launcher.request_->executable != updaterExe);
    LAU_REQUIRE(launcher.request_->executable.parent_path() == tempDir / "launcher");
    LAU_REQUIRE(std::filesystem::exists(launcher.request_->executable));
    LAU_REQUIRE(readFile(launcher.request_->executable) == "updater-binary");

    std::filesystem::remove_all(root, ec);
}

void testApplyLauncherRejectsMissingUpdaterExecutable() {
    const auto root = uniqueTempDir();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    autoupdater::Config config;
    config.installDir = root / "install";
    config.tempDir = root / "staging";
    config.updaterExecutable = config.installDir / "missing-updater.exe";

    CapturingProcessLauncher launcher;
    auto launched = autoupdater::launchApplyProcess(config, config.tempDir / "apply-plan.json", launcher);
    LAU_REQUIRE(!launched);
    LAU_REQUIRE(launched.error().code == autoupdater::ErrorCode::ApplyLaunchFailed);
    LAU_REQUIRE(!launcher.request_.has_value());

    std::filesystem::remove_all(root, ec);
}
