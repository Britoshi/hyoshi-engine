#include "core/Result.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>

using hyoshi::Error;
using hyoshi::Result;

namespace
{

Result<int> ParsePositive(int value)
{
    if (value <= 0)
    {
        return Error{"not positive"};
    }
    return value;
}

Result<void> Check(bool ok)
{
    if (!ok)
    {
        return Error{"check failed"};
    }
    return {};
}

} // namespace

TEST_CASE("Result holds a value on success")
{
    Result<int> result = ParsePositive(42);
    REQUIRE(result.HasValue());
    CHECK(static_cast<bool>(result));
    CHECK(result.Value() == 42);
}

TEST_CASE("Result holds an error on failure")
{
    Result<int> result = ParsePositive(-1);
    REQUIRE_FALSE(result.HasValue());
    CHECK(result.GetError().Message == "not positive");
}

TEST_CASE("Result supports move-only values")
{
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
    REQUIRE(result.HasValue());
    std::unique_ptr<int> value = std::move(result).Value();
    CHECK(*value == 7);
}

TEST_CASE("Result<void> distinguishes success from failure")
{
    CHECK(Check(true).HasValue());

    Result<void> failure = Check(false);
    REQUIRE_FALSE(failure.HasValue());
    CHECK(failure.GetError().Message == "check failed");
}
