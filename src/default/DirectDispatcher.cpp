#include "libAutoUpdater/interfaces/IEventDispatcher.h"

namespace autoupdater {

namespace {

class DirectDispatcher final : public IEventDispatcher {
  public:
    void post(std::function<void()> fn) noexcept override {
        if (fn) {
            fn();
        }
    }
};

} // namespace

std::shared_ptr<IEventDispatcher> createDirectDispatcher() {
    return std::make_shared<DirectDispatcher>();
}

} // namespace autoupdater
