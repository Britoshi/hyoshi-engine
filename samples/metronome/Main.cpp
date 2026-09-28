// A minimal game on Hyoshi Engine: the splash, then the clock demo, a metronome whose beat flash
// follows the audio clock. F1 shows the clock's debug panel.

#include "app/Application.h"
#include "app/ClockDemo.h"
#include "core/Log.h"
#include "platform/EntryPoint.h"

#include <memory>

namespace
{

class MetronomeGame : public hyoshi::app::Game
{
public:
    hyoshi::Result<void> Initialize(hyoshi::app::AppServices& appServices) override
    {
        services = &appServices;
        return {};
    }

    std::unique_ptr<hyoshi::app::Scene> CreateFirstScene() override
    {
        auto demo = std::make_unique<hyoshi::app::ClockDemo>(services->Audio, services->Jobs);
        demo->Start();
        return demo;
    }

private:
    hyoshi::app::AppServices* services = nullptr;
};

} // namespace

int main(int, char*[])
{
    hyoshi::log::Initialize();
    int exitCode = 0;
    {
        MetronomeGame game;
        hyoshi::app::AppConfig config;
        config.Name = "Hyoshi Metronome";
        hyoshi::app::Application application(config, game);
        exitCode = application.Run();
    }
    hyoshi::log::Shutdown();
    return exitCode;
}
