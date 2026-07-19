#include "LocalSnapshotBuilder.h"

#include <set>

namespace autoupdater {

Result<LocalSnapshot> buildLocalSnapshot(const Config& config, const Manifest& manifest, IFileSystem& fileSystem,
                                         IHashProvider& hashProvider) {
    LocalSnapshot snapshot;
    std::set<std::string> seen;
    auto root = fileSystem.openRoot(config.installDir, RootAccess::ReadOnly, false);
    if (!root) {
        return Result<LocalSnapshot>::fail(root.error());
    }

    for (const auto& file : manifest.files) {
        const auto localPath = file.localPath.empty() ? file.path : file.localPath;
        if (!seen.insert(localPath).second) {
            continue;
        }

        LocalFileInfo info;
        info.path = localPath;
        auto opened = root.value()->openRegularFile(localPath, RootedFileOpenMode::ReadOnly);
        if (!opened) {
            return Result<LocalSnapshot>::fail(opened.error());
        }
        info.exists = opened.value().exists();
        if (opened.value().exists()) {
            auto hash = hashProvider.sha256Stream(*opened.value().file);
            if (!hash) {
                return Result<LocalSnapshot>::fail(hash.error());
            }
            auto metadata = opened.value().file->metadata();
            if (!metadata) {
                return Result<LocalSnapshot>::fail(metadata.error());
            }
            info.sha256 = hash.value();
            info.size = metadata.value().size;
        }
        snapshot.files.push_back(std::move(info));
    }

    return Result<LocalSnapshot>::ok(std::move(snapshot));
}

} // namespace autoupdater
