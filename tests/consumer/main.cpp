#include <libAutoUpdater/Updater.h>
#include <libAutoUpdater/Version.h>

int main() {
    auto version = autoupdater::Version::parse("1.0.0");
    if (!version) {
        return 1;
    }

    autoupdater::Config config;
    config.currentVersion = version.value();
    return config.currentVersion.toString() == "1.0.0" ? 0 : 2;
}

