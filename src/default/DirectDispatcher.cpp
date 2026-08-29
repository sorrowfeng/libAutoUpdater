#include "libAutoUpdater/interfaces/IEventDispatcher.h"

namespace autoupdater {

namespace {

class DirectDispatcher final : public IEventDispatcher {
  public:
    void post(std::function<void()> fn) noexcept override {
        // The dispatcher contract is noexcept; a throwing user callback must
        // not terminate the host process.
        try {
            if (fn) {
                fn();
            }
        } catch (...) {
        }
    }
};

} // namespace

std::shared_ptr<IEventDispatcher> createDirectDispatcher() {
    return std::make_shared<DirectDispatcher>();
}

} // namespace autoupdater
