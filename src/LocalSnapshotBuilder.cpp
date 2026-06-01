#include "LocalSnapshotBuilder.h"

#include "util/PathUtil.h"

#include <set>

namespace autoupdater {

Result<LocalSnapshot> buildLocalSnapshot(const Config& config,
                                         const Manifest& manifest,
                                         IFileSystem& fileSystem,
                                         IHashProvider& hashProvider) {
    LocalSnapshot snapshot;
    std::set<std::string> seen;

    for (const auto& file : manifest.files) {
        const auto localPath = file.localPath.empty() ? file.path : file.localPath;
        if (!seen.insert(localPath).second) {
            continue;
        }

        auto fullPath = util::safeJoin(config.installDir, localPath);
        if (!fullPath) {
            return Result<LocalSnapshot>::fail(fullPath.error());
        }

        LocalFileInfo info;
        info.path = localPath;
        info.exists = fileSystem.exists(fullPath.value()) && fileSystem.isRegularFile(fullPath.value());
        if (info.exists) {
            auto hash = hashProvider.sha256File(fullPath.value());
            if (!hash) {
                return Result<LocalSnapshot>::fail(hash.error());
            }
            auto size = fileSystem.fileSize(fullPath.value());
            if (!size) {
                return Result<LocalSnapshot>::fail(size.error());
            }
            info.sha256 = hash.value();
            info.size = size.value();
        }
        snapshot.files.push_back(std::move(info));
    }

    return Result<LocalSnapshot>::ok(std::move(snapshot));
}

} // namespace autoupdater

