#pragma once

#include "libAutoUpdater/Error.h"

#include <cassert>
#include <utility>

namespace autoupdater {

template <class T> class [[nodiscard]] Result {
  public:
    static Result ok(T value) {
        Result result;
        result.hasValue_ = true;
        result.value_ = std::move(value);
        return result;
    }

    static Result fail(Error error) {
        Result result;
        result.hasValue_ = false;
        result.error_ = std::move(error);
        return result;
    }

    bool hasValue() const noexcept {
        return hasValue_;
    }
    explicit operator bool() const noexcept {
        return hasValue_;
    }

    const T& value() const {
        assert(hasValue_ && "Result::value() called on a failed Result");
        return value_;
    }
    T& value() {
        assert(hasValue_ && "Result::value() called on a failed Result");
        return value_;
    }

    const Error& error() const noexcept {
        return error_;
    }

  private:
    bool hasValue_ = false;
    T value_{};
    Error error_{ErrorCode::InternalError, "Result has no value"};
};

template <> class [[nodiscard]] Result<void> {
  public:
    static Result ok() {
        Result result;
        result.hasValue_ = true;
        result.error_ = Error{};
        return result;
    }

    static Result fail(Error error) {
        Result result;
        result.hasValue_ = false;
        result.error_ = std::move(error);
        return result;
    }

    bool hasValue() const noexcept {
        return hasValue_;
    }
    explicit operator bool() const noexcept {
        return hasValue_;
    }

    const Error& error() const noexcept {
        return error_;
    }

  private:
    bool hasValue_ = false;
    Error error_{ErrorCode::InternalError, "Result has no value"};
};

} // namespace autoupdater
