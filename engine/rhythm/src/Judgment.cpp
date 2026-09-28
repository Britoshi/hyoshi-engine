#include "rhythm/Judgment.h"

namespace hyoshi::rhythm
{

const char* ToString(Judgment judgment)
{
    switch (judgment)
    {
    case Judgment::Perfect:
        return "Perfect";
    case Judgment::Great:
        return "Great";
    case Judgment::Good:
        return "Good";
    case Judgment::Bad:
        return "Bad";
    case Judgment::Miss:
        return "Miss";
    case Judgment::Count:
        break;
    }
    return "?";
}

std::optional<Judgment> HitWindows::Classify(int64_t errorUs) const
{
    const int64_t distance = errorUs < 0 ? -errorUs : errorUs;
    if (distance <= PerfectUs)
    {
        return Judgment::Perfect;
    }
    if (distance <= GreatUs)
    {
        return Judgment::Great;
    }
    if (distance <= GoodUs)
    {
        return Judgment::Good;
    }
    if (distance <= BadUs)
    {
        return Judgment::Bad;
    }
    return std::nullopt;
}

} // namespace hyoshi::rhythm
