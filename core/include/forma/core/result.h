#pragma once

#include <string>
#include <utility>
#include <variant>

namespace forma {

struct Error {
    std::string code;
    std::string message;
};

inline Error makeError(std::string code, std::string message)
{
    return Error{std::move(code), std::move(message)};
}

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return state_.index() == 0; }
    explicit operator bool() const { return ok(); }

    const T& value() const& { return std::get<0>(state_); }
    T& value() & { return std::get<0>(state_); }
    T&& value() && { return std::get<0>(std::move(state_)); }

    const T* operator->() const { return &std::get<0>(state_); }
    T* operator->() { return &std::get<0>(state_); }
    const T& operator*() const& { return std::get<0>(state_); }
    T& operator*() & { return std::get<0>(state_); }

    const Error& error() const { return std::get<1>(state_); }

    T valueOr(T fallback) const&
    {
        return ok() ? std::get<0>(state_) : std::move(fallback);
    }

private:
    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), failed_(true) {}

    bool ok() const { return !failed_; }
    explicit operator bool() const { return ok(); }
    const Error& error() const { return error_; }

private:
    Error error_;
    bool failed_ = false;
};

using Status = Result<void>;

}
