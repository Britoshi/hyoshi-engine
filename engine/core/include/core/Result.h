#pragma once

#include "core/Assert.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace hyoshi
{

struct Error
{
    std::string Message;
};

// Either a value or an Error. Fallible operations return this instead of throwing.
//
//     Result<Texture> LoadTexture(...);
//     return Error{"file not found"};   // failure
//     return texture;                   // success
template <typename T>
class [[nodiscard]] Result
{
public:
    Result(T value) : storage(std::in_place_index<0>, std::move(value))
    {
    }

    Result(Error failure) : storage(std::in_place_index<1>, std::move(failure))
    {
    }

    bool HasValue() const
    {
        return storage.index() == 0;
    }

    explicit operator bool() const
    {
        return HasValue();
    }

    T& Value() &
    {
        HYOSHI_ASSERT(HasValue(), "Result holds an error");
        return std::get<0>(storage);
    }

    const T& Value() const&
    {
        HYOSHI_ASSERT(HasValue(), "Result holds an error");
        return std::get<0>(storage);
    }

    T&& Value() &&
    {
        HYOSHI_ASSERT(HasValue(), "Result holds an error");
        return std::get<0>(std::move(storage));
    }

    const Error& GetError() const
    {
        HYOSHI_ASSERT(!HasValue(), "Result holds a value");
        return std::get<1>(storage);
    }

private:
    std::variant<T, Error> storage;
};

// Success carries no value: `return {};` succeeds, `return Error{...};` fails.
template <>
class [[nodiscard]] Result<void>
{
public:
    Result() = default;

    Result(Error failure) : error(std::move(failure))
    {
    }

    bool HasValue() const
    {
        return !error.has_value();
    }

    explicit operator bool() const
    {
        return HasValue();
    }

    const Error& GetError() const
    {
        HYOSHI_ASSERT(!HasValue(), "Result holds a value");
        return *error;
    }

private:
    std::optional<Error> error;
};

} // namespace hyoshi
