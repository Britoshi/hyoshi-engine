#pragma once

#include "core/Result.h"
#include "rhi/IRenderDevice.h"

#include <imgui.h>

#include <array>
#include <string>
#include <vector>

namespace hyoshi::platform
{
class Platform;
}

namespace hyoshi::debug
{

// Dear ImGui for debug overlays and tools, never player-facing UI (DESIGN.md section 17.3).
// Input comes from ImGui's SDL3 backend; drawing goes through the RHI like everything else.
class DebugUi
{
public:
    Result<void> Initialize(rhi::IRenderDevice& device, platform::Platform& platform);
    void Shutdown();

    // Starts an ImGui frame. Build windows with ImGui:: calls between this and Render.
    void BeginFrame();

    // Draws the frame's UI. Call inside a render pass.
    void Render(rhi::ICommandList& commands);

    // Whether ImGui is using the keyboard or pointer, so gameplay should ignore it.
    bool WantsKeyboard() const;
    bool WantsPointer() const;

private:
    void UpdateTexture(ImTextureData& texture);
    Result<void> EnsureCapacity(std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT>& buffers,
                                std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT>& capacities, uint32_t frame, size_t bytes,
                                rhi::BufferUsage usage);

    rhi::IRenderDevice* device = nullptr;
    platform::Platform* platform = nullptr;
    rhi::PipelineHandle pipeline;
    rhi::SamplerHandle sampler;

    std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> vertexBuffers{};
    std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT> vertexCapacities{};
    std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT> indexBuffers{};
    std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT> indexCapacities{};

    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<unsigned char> textureRegion;
    std::string iniPath;
    bool isInitialized = false;
};

} // namespace hyoshi::debug
