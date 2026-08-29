#pragma once

#include <functional>
#include <memory>

namespace autoupdater {

/// Dispatches callbacks onto the caller's preferred execution context.
///
/// Implementations must honor the noexcept contract: a throwing callback must
/// be contained inside post(). The default direct dispatcher executes the
/// callback synchronously on the updater's worker thread and swallows
/// exceptions; GUI integrations should provide a dispatcher that marshals
/// callbacks to the UI thread.
class IEventDispatcher {
  public:
    virtual ~IEventDispatcher() = default;
    virtual void post(std::function<void()> fn) noexcept = 0;
};

std::shared_ptr<IEventDispatcher> createDirectDispatcher();

} // namespace autoupdater
