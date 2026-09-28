#include "core/Handle.h"

#include <doctest/doctest.h>

#include <string>

using hyoshi::Handle;
using hyoshi::HandlePool;

namespace
{

struct TextureTag;
struct BufferTag;

} // namespace

TEST_CASE("Default-constructed handles are invalid")
{
    Handle<TextureTag> handle;
    CHECK_FALSE(handle.IsValid());
}

TEST_CASE("HandlePool creates, gets, and destroys objects")
{
    HandlePool<std::string> pool;

    auto first = pool.Create("first");
    auto second = pool.Create("second");
    CHECK(first.IsValid());
    CHECK(pool.Size() == 2);
    REQUIRE(pool.Get(first) != nullptr);
    CHECK(*pool.Get(first) == "first");
    CHECK(*pool.Get(second) == "second");

    CHECK(pool.Destroy(first));
    CHECK(pool.Size() == 1);
    CHECK(pool.Get(first) == nullptr);
    CHECK_FALSE(pool.Contains(first));
    CHECK_FALSE(pool.Destroy(first));
}

TEST_CASE("A stale handle does not alias a reused slot")
{
    HandlePool<int> pool;

    auto stale = pool.Create(1);
    REQUIRE(pool.Destroy(stale));

    auto reused = pool.Create(2);
    CHECK(reused.Index == stale.Index);
    CHECK(reused.Generation != stale.Generation);
    CHECK(pool.Get(stale) == nullptr);
    REQUIRE(pool.Get(reused) != nullptr);
    CHECK(*pool.Get(reused) == 2);
}

TEST_CASE("Handles from an empty or foreign pool are rejected")
{
    HandlePool<int, BufferTag> pool;
    CHECK(pool.Get(Handle<BufferTag>{}) == nullptr);
    CHECK(pool.Get(Handle<BufferTag>{5, 1}) == nullptr);
}

TEST_CASE("HandlePool visits live objects and clears them")
{
    HandlePool<int> pool;
    auto first = pool.Create(1);
    auto second = pool.Create(2);
    auto third = pool.Create(3);
    REQUIRE(pool.Destroy(second));

    int sum = 0;
    pool.ForEach([&sum](int& value) { sum += value; });
    CHECK(sum == 4);

    pool.Clear();
    CHECK(pool.IsEmpty());
    CHECK(pool.Get(first) == nullptr);
    CHECK(pool.Get(third) == nullptr);

    auto reused = pool.Create(5);
    CHECK(pool.Size() == 1);
    CHECK(*pool.Get(reused) == 5);
}
