#include "rhythm/OffsetCalibration.h"

#include <doctest/doctest.h>

#include <cstdint>

using hyoshi::rhythm::OffsetCalibration;

TEST_CASE("OffsetCalibration suggests the offset that centers recent hits")
{
    OffsetCalibration calibration;
    for (size_t i = 0; i + 1 < OffsetCalibration::MIN_HITS; ++i)
    {
        calibration.Add(20'000, 0);
    }
    // Not enough hits yet.
    CHECK_FALSE(calibration.SuggestOffset());

    // A player 20 ms late with no offset needs +20 ms.
    calibration.Add(20'000, 0);
    REQUIRE(calibration.SuggestOffset());
    CHECK(*calibration.SuggestOffset() == 20'000);

    // Hits made after the offset already moved by 15 ms count by what they'd have been without it.
    calibration.Clear();
    CHECK(calibration.GetCount() == 0);
    for (size_t i = 0; i < OffsetCalibration::MIN_HITS; ++i)
    {
        calibration.Add(i % 2 == 0 ? 20'000 : 5'000, i % 2 == 0 ? 0 : 15'000);
    }
    CHECK(*calibration.SuggestOffset() == 20'000);
}

TEST_CASE("OffsetCalibration uses the median of the last hits")
{
    OffsetCalibration calibration;
    // Stale hits, pushed out by the newest MAX_HITS.
    for (int i = 0; i < 50; ++i)
    {
        calibration.Add(-80'000, 0);
    }
    for (size_t i = 0; i < OffsetCalibration::MAX_HITS; ++i)
    {
        // Mostly 10 ms late, with a few wild presses that a mean would follow.
        calibration.Add(i % 10 == 0 ? 100'000 : 10'000, 0);
    }
    CHECK(calibration.GetCount() == OffsetCalibration::MAX_HITS);
    CHECK(*calibration.SuggestOffset() == 10'000);

    // Even counts take the midpoint of the middle two.
    OffsetCalibration even;
    for (size_t i = 0; i < OffsetCalibration::MIN_HITS; ++i)
    {
        even.Add(i < OffsetCalibration::MIN_HITS / 2 ? -4'000 : 6'000, 0);
    }
    CHECK(*even.SuggestOffset() == 1'000);
}
