#pragma once

#include <functional>
#include <memory>

namespace autoupdater {

/// Dispatches callbacks onto the caller's preferred execution context.
class IEventDispatcher {
  public:
    virtual ~IEventDispatcher() = default;
    virtual void post(std::function<void()> fn) noexcept = 0;
};

std::shared_ptr<IEventDispatcher> createDirectDispatcher();

} // namespace autoupdater
