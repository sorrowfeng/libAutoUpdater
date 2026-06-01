#include "libAutoUpdater/interfaces/IHashProvider.h"

#include "util/Sha256.h"

namespace autoupdater {

namespace {

class Sha256HashProvider final : public IHashProvider {
public:
    Result<std::string> sha256File(const std::filesystem::path& path) noexcept override {
        return util::sha256File(path);
    }

    Result<std::string> sha256Bytes(std::string_view data) noexcept override {
        return Result<std::string>::ok(util::sha256Bytes(data));
    }
};

} // namespace

std::shared_ptr<IHashProvider> createDefaultHashProvider() {
    return std::make_shared<Sha256HashProvider>();
}

} // namespace autoupdater

