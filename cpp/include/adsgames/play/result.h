/**
 * Result of a play request
 */

#pragma once

#include <optional>
#include <string>
#include <utility>

namespace adsgames::play {

template <typename T> class Result {
public:
    static Result success(T value, int status = 200)
    {
        Result result;
        result.value_ = std::move(value);
        result.status_ = status;
        return result;
    }

    // status is the HTTP status, or 0 when play could not be reached
    static Result failure(int status, std::string error)
    {
        Result result;
        result.status_ = status;
        result.error_ = std::move(error);
        return result;
    }

    bool ok() const
    {
        return value_.has_value();
    }

    explicit operator bool() const
    {
        return ok();
    }

    // Only valid when ok(), like std::optional
    const T& value() const
    {
        return *value_; // NOLINT(bugprone-unchecked-optional-access)
    }

    const T* operator->() const
    {
        return &*value_; // NOLINT(bugprone-unchecked-optional-access)
    }

    // HTTP status, 0 when play could not be reached
    int status() const
    {
        return status_;
    }

    const std::string& error() const
    {
        return error_;
    }

    // The player is a guest, scores and unlocks need an adsgames.net login
    bool not_logged_in() const
    {
        return status_ == 401;
    }

private:
    Result() = default;

    std::optional<T> value_;
    int status_ { 0 };
    std::string error_;
};

} // namespace adsgames::play
