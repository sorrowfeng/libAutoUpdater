#include "libAutoUpdater/interfaces/IEventDispatcher.h"

namespace autoupdater {

namespace {

class DirectDispatcher final : public IEventDispatcher {
  public:
    void post(std::function<void()> fn) noexcept override {
        if (fn) {
            try {
                fn();
            } catch (...) {
                // User callbacks must never escape a noexcept dispatch boundary.
            }
        }
    }
};

} // namespace

std::shared_ptr<IEventDispatcher> createDirectDispatcher() {
    return std::make_shared<DirectDispatcher>();
}

} // namespace autoupdater
