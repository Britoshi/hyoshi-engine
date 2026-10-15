#pragma once

#include "app/Scene.h"
#include "audio/IAudioBackend.h"
#include "rhi/IRenderDevice.h"
#include "ui/Logo.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace hyoshi::app
{

// "Made with Hyoshi Engine", on a beat: the mark's four tiles land one a beat, each lighting a
// dash of a line under it with a knock; on the fifth beat the dashes join with a flash and
// sparks, and the wordmark slides out from behind the mark. Then the game's first scene. Any key
// or tap skips it. Its sounds play on the UI bus, so a game sets that bus's volume.
class SplashScene : public Scene
{
public:
    using NextScene = std::function<std::unique_ptr<Scene>()>;

    SplashScene(const ui::Logo& wordmark, rhi::IRenderDevice& device, audio::IAudioBackend& audio, NextScene next);
    ~SplashScene() override;
    SplashScene(const SplashScene&) = delete;
    SplashScene& operator=(const SplashScene&) = delete;

    void RunFrame(const FrameContext& frame, ui::Ui& ui) override;

private:
    // Plays each beat's sound as it comes due.
    void PlayBeats();

    const ui::Logo& wordmark;
    rhi::IRenderDevice& device;
    audio::IAudioBackend& audio;
    NextScene next;
    // A soft white blob (glows, sparks) and a soft ring (the hit's wave).
    rhi::TextureHandle glow;
    rhi::TextureHandle ring;
    bool areSoundsLoaded = false;
    int32_t beatsPlayed = 0;
    float age = 0.0f;
    float leaveAge = -1.0f;
};

} // namespace hyoshi::app
