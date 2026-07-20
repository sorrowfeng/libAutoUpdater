#include "QtDispatcher.h"
#include "QtNetworkClient.h"

#include "libAutoUpdater/Updater.h"

#include "util/PathUtil.h"

#include <QCoreApplication>
#include <QDebug>

#include <string>

namespace {

std::string toUtf8String(const QString& value) {
    const auto bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();

    if (args.size() < 5) {
        qInfo() << "Usage: libAutoUpdater_qt <manifest-url> <current-version> <install-dir> <allowed-base-url>";
        return 2;
    }

    auto version = autoupdater::Version::parse(toUtf8String(args.at(2)));
    if (!version) {
        qWarning() << QString::fromStdString(autoupdater::formatDiagnostic(version.error()));
        return 2;
    }

    autoupdater::Config config;
    config.manifestUrl = toUtf8String(args.at(1));
    config.currentVersion = version.value();
    config.installDir = autoupdater::util::pathFromUtf8(toUtf8String(args.at(3)));
    config.security.allowedBaseUrls.push_back(toUtf8String(args.at(4)));

    auto dispatcher = std::make_shared<QtDispatcher>();
    auto network = std::make_shared<QtNetworkClient>();

    autoupdater::Updater updater(config);
    updater.setEventDispatcher(dispatcher);
    updater.setNetworkClient(network);

    autoupdater::Callbacks callbacks;
    callbacks.onCheckResult = [](const autoupdater::CheckResult& result) {
        qInfo() << "updateAvailable=" << result.updateAvailable;
    };
    callbacks.onProgress = [](const autoupdater::Progress& progress) {
        qInfo() << "progress" << progress.downloadedBytes << "/" << progress.totalBytes;
    };
    callbacks.onReadyToApply = [&] {
        qInfo() << "ready to apply";
        QCoreApplication::quit();
    };
    callbacks.onError = [&](const autoupdater::Error& error) {
        qWarning() << QString::fromStdString(autoupdater::formatDiagnostic(error));
        QCoreApplication::exit(1);
    };
    updater.setCallbacks(std::move(callbacks));
    updater.checkAndDownloadAsync();

    return app.exec();
}
