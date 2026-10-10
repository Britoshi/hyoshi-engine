#pragma once

#include "audio/AudioTypes.h"
#include "circle/CircleChart.h"

#include <cstdint>

namespace hyoshi::songs::detail
{

// The sounds of a sample bank, as osu! names them (soft-hitclap.wav).
enum class HitSoundName : uint8_t
{
    HitNormal,
    HitWhistle,
    HitFinish,
    HitClap,
    SliderTick
};

const char* GetBankFileName(circle::SampleBank bank);
const char* GetHitSoundFileName(HitSoundName name);

// A generated stand-in for osu!'s default sound, which the engine can't ship: a click or a knock
// for the normal sound, a whistle, a cymbal crash, a hand clap (a snare in the drum bank), or a
// short tick. Soft is duller and quieter than normal; drum is lower, a kick and toms.
audio::PcmBuffer MakeDefaultHitSound(circle::SampleBank bank, HitSoundName name, uint32_t sampleRate);

} // namespace hyoshi::songs::detail
