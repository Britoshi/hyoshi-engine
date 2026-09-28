#include "debug/DebugUi.h"

#include "DebugUi.spv.h"

#include "core/Log.h"
#include "platform/Platform.h"
#include "renderer/Camera2D.h"

#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace hyoshi::debug
{

namespace
{

constexpr size_t INITIAL_VERTEX_BYTES = 64 * 1024;
constexpr size_t INITIAL_INDEX_BYTES = 32 * 1024;

ImTextureID ToTextureId(rhi::TextureHandle handle)
{
    return (static_cast<ImTextureID>(handle.Generation) << 32) | handle.Index;
}

rhi::TextureHandle ToTextureHandle(ImTextureID id)
{
    return {static_cast<uint32_t>(id & 0xFFFFFFFFu), static_cast<uint32_t>(id >> 32)};
}

// Empty on purpose: registered as ImGui's "reset render state" callback and recognized by address
// in the draw loop.
void ResetRenderStateCallback(const ImDrawList*, const ImDrawCmd*)
{
}

} // namespace

Result<void> DebugUi::Initialize(rhi::IRenderDevice& renderDevice, platform::Platform& targetPlatform)
{
    device = &renderDevice;
    platform = &targetPlatform;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "hyoshi_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    // Window layout persists in the user data directory rather than the working directory.
    iniPath = platform->GetUserDataPath().empty() ? std::string() : platform->GetUserDataPath() + "imgui.ini";
    io.IniFilename = iniPath.empty() ? nullptr : iniPath.c_str();

    ImGui::StyleColorsDark();
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = ResetRenderStateCallback;

    if (!ImGui_ImplSDL3_InitForVulkan(platform->GetNativeWindow()))
    {
        return Error{"ImGui_ImplSDL3_InitForVulkan failed"};
    }
    isInitialized = true;
    platform->SetEventHook([](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); });

    rhi::PipelineDesc pipelineDesc;
    pipelineDesc.Spirv = DEBUG_UI_SPIRV;
    pipelineDesc.Blend = rhi::BlendMode::Alpha;
    pipelineDesc.PushConstantSize = sizeof(renderer::ClipTransform);
    pipelineDesc.TextureCount = 1;
    pipelineDesc.VertexBindings = {{0, sizeof(ImDrawVert), rhi::VertexInputRate::Vertex}};
    pipelineDesc.VertexAttributes = {
        {0, 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(ImDrawVert, pos))},
        {1, 0, rhi::VertexFormat::Float2, static_cast<uint32_t>(offsetof(ImDrawVert, uv))},
        {2, 0, rhi::VertexFormat::UByte4Norm, static_cast<uint32_t>(offsetof(ImDrawVert, col))},
    };
    pipelineDesc.DebugName = "DebugUi";

    Result<rhi::PipelineHandle> createdPipeline = device->CreatePipeline(pipelineDesc);
    if (!createdPipeline)
    {
        return createdPipeline.GetError();
    }
    pipeline = createdPipeline.Value();

    Result<rhi::SamplerHandle> createdSampler = device->CreateSampler({});
    if (!createdSampler)
    {
        return createdSampler.GetError();
    }
    sampler = createdSampler.Value();

    return {};
}

void DebugUi::Shutdown()
{
    if (!isInitialized)
    {
        return;
    }

    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
    {
        if (texture->TexID != ImTextureID_Invalid)
        {
            device->Destroy(ToTextureHandle(texture->TexID));
            texture->SetTexID(ImTextureID_Invalid);
        }
        texture->SetStatus(ImTextureStatus_Destroyed);
    }

    for (uint32_t frame = 0; frame < rhi::MAX_FRAMES_IN_FLIGHT; ++frame)
    {
        device->Destroy(vertexBuffers[frame]);
        device->Destroy(indexBuffers[frame]);
    }
    device->Destroy(sampler);
    device->Destroy(pipeline);

    platform->SetEventHook(nullptr);
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    isInitialized = false;
}

void DebugUi::BeginFrame()
{
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

bool DebugUi::WantsKeyboard() const
{
    return isInitialized && ImGui::GetIO().WantCaptureKeyboard;
}

bool DebugUi::WantsPointer() const
{
    return isInitialized && ImGui::GetIO().WantCaptureMouse;
}

void DebugUi::Render(rhi::ICommandList& commands)
{
    ImGui::Render();
    const ImDrawData* drawData = ImGui::GetDrawData();

    // ImGui 1.92 grows and edits its font atlas on demand; apply its requests before drawing.
    if (drawData->Textures != nullptr)
    {
        for (ImTextureData* texture : *drawData->Textures)
        {
            if (texture->Status != ImTextureStatus_OK)
            {
                UpdateTexture(*texture);
            }
        }
    }

    const ImVec2 scale = drawData->FramebufferScale;
    const float framebufferWidth = drawData->DisplaySize.x * scale.x;
    const float framebufferHeight = drawData->DisplaySize.y * scale.y;
    if (framebufferWidth <= 0.0f || framebufferHeight <= 0.0f || drawData->TotalVtxCount == 0)
    {
        return;
    }

    vertices.clear();
    indices.clear();
    for (const ImDrawList* list : drawData->CmdLists)
    {
        vertices.insert(vertices.end(), list->VtxBuffer.begin(), list->VtxBuffer.end());
        indices.insert(indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
    }

    const uint32_t frame = device->GetFrameIndex();
    const size_t vertexBytes = vertices.size() * sizeof(ImDrawVert);
    const size_t indexBytes = indices.size() * sizeof(ImDrawIdx);
    if (!EnsureCapacity(vertexBuffers, vertexCapacities, frame, vertexBytes, rhi::BufferUsage::Vertex) ||
        !EnsureCapacity(indexBuffers, indexCapacities, frame, indexBytes, rhi::BufferUsage::Index) ||
        !device->UploadBuffer(vertexBuffers[frame], vertices.data(), vertexBytes, 0) ||
        !device->UploadBuffer(indexBuffers[frame], indices.data(), indexBytes, 0))
    {
        HYOSHI_LOG_ERROR("DebugUi: could not upload draw data");
        return;
    }

    const rhi::SurfaceTransform transform = device->GetSurfaceTransform();
    const rhi::Extent swapchainExtent = device->GetSwapchainExtent();
    const rhi::Extent logicalExtent = renderer::GetLogicalExtent(swapchainExtent, transform);

    // The window can be a frame ahead of the swapchain during a resize, so clip to both.
    const float clipWidth = std::min(framebufferWidth, static_cast<float>(logicalExtent.Width));
    const float clipHeight = std::min(framebufferHeight, static_cast<float>(logicalExtent.Height));

    // Vertices are in logical points starting at DisplayPos.
    renderer::ClipTransform clip =
        renderer::MakePixelClipTransform({drawData->DisplaySize.x, drawData->DisplaySize.y}, transform);
    clip.Offset -= glm::vec2(drawData->DisplayPos.x, drawData->DisplayPos.y) * clip.Scale;

    const auto bindRenderState = [&]
    {
        commands.BindPipeline(pipeline);
        commands.PushConstants(&clip, sizeof(clip));
        commands.BindVertexBuffer(0, vertexBuffers[frame], 0);
        commands.BindIndexBuffer(indexBuffers[frame], 0,
                                 sizeof(ImDrawIdx) == 2 ? rhi::IndexType::UInt16 : rhi::IndexType::UInt32);
    };
    bindRenderState();

    uint32_t globalVertexOffset = 0;
    uint32_t globalIndexOffset = 0;
    for (const ImDrawList* list : drawData->CmdLists)
    {
        for (const ImDrawCmd& command : list->CmdBuffer)
        {
            if (command.UserCallback != nullptr)
            {
                if (command.UserCallback == ResetRenderStateCallback)
                {
                    bindRenderState();
                }
                else
                {
                    command.UserCallback(list, &command);
                }
                continue;
            }

            // Clip rectangle in framebuffer pixels, clamped to the framebuffer.
            const float minX = std::max((command.ClipRect.x - drawData->DisplayPos.x) * scale.x, 0.0f);
            const float minY = std::max((command.ClipRect.y - drawData->DisplayPos.y) * scale.y, 0.0f);
            const float maxX = std::min((command.ClipRect.z - drawData->DisplayPos.x) * scale.x, clipWidth);
            const float maxY = std::min((command.ClipRect.w - drawData->DisplayPos.y) * scale.y, clipHeight);
            if (maxX <= minX || maxY <= minY)
            {
                continue;
            }

            const rhi::Rect logicalRect{static_cast<int32_t>(minX), static_cast<int32_t>(minY),
                                        static_cast<uint32_t>(maxX - minX), static_cast<uint32_t>(maxY - minY)};
            commands.SetScissor(renderer::ToSwapchainRect(logicalRect, logicalExtent, transform));
            commands.BindTexture(0, ToTextureHandle(command.GetTexID()), sampler);
            commands.DrawIndexed(command.ElemCount, 1, command.IdxOffset + globalIndexOffset,
                                 static_cast<int32_t>(command.VtxOffset + globalVertexOffset), 0);
        }
        globalVertexOffset += static_cast<uint32_t>(list->VtxBuffer.Size);
        globalIndexOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
    }

    commands.SetScissor({0, 0, swapchainExtent.Width, swapchainExtent.Height});
}

void DebugUi::UpdateTexture(ImTextureData& texture)
{
    const auto width = static_cast<uint32_t>(texture.Width);
    const auto height = static_cast<uint32_t>(texture.Height);

    if (texture.Status == ImTextureStatus_WantCreate)
    {
        rhi::TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format =
            texture.Format == ImTextureFormat_Alpha8 ? rhi::TextureFormat::R8Unorm : rhi::TextureFormat::Rgba8Unorm;
        desc.DebugName = "ImGui texture";

        Result<rhi::TextureHandle> created = device->CreateTexture(desc);
        if (!created)
        {
            HYOSHI_LOG_ERROR("DebugUi: {}", created.GetError().Message);
            return;
        }
        if (Result<void> result = device->UpdateTexture(created.Value(), texture.GetPixels(), 0, 0, width, height);
            !result)
        {
            HYOSHI_LOG_ERROR("DebugUi: {}", result.GetError().Message);
        }
        texture.SetTexID(ToTextureId(created.Value()));
        texture.SetStatus(ImTextureStatus_OK);
    }
    else if (texture.Status == ImTextureStatus_WantUpdates)
    {
        // Upload the bounding box of all queued updates, repacked into tight rows.
        const ImTextureRect& rect = texture.UpdateRect;
        const size_t rowBytes = size_t{rect.w} * static_cast<size_t>(texture.BytesPerPixel);
        textureRegion.resize(rowBytes * rect.h);
        for (int row = 0; row < rect.h; ++row)
        {
            std::memcpy(textureRegion.data() + static_cast<size_t>(row) * rowBytes,
                        texture.GetPixelsAt(rect.x, rect.y + row), rowBytes);
        }

        if (Result<void> result = device->UpdateTexture(ToTextureHandle(texture.TexID), textureRegion.data(), rect.x,
                                                        rect.y, rect.w, rect.h);
            !result)
        {
            HYOSHI_LOG_ERROR("DebugUi: {}", result.GetError().Message);
        }
        texture.SetStatus(ImTextureStatus_OK);
    }
    else if (texture.Status == ImTextureStatus_WantDestroy)
    {
        // The RHI defers the actual deletion until in-flight frames are done with it.
        device->Destroy(ToTextureHandle(texture.TexID));
        texture.SetTexID(ImTextureID_Invalid);
        texture.SetStatus(ImTextureStatus_Destroyed);
    }
}

Result<void> DebugUi::EnsureCapacity(std::array<rhi::BufferHandle, rhi::MAX_FRAMES_IN_FLIGHT>& buffers,
                                     std::array<size_t, rhi::MAX_FRAMES_IN_FLIGHT>& capacities, uint32_t frame,
                                     size_t bytes, rhi::BufferUsage usage)
{
    if (bytes <= capacities[frame])
    {
        return {};
    }

    size_t capacity =
        std::max(capacities[frame], usage == rhi::BufferUsage::Vertex ? INITIAL_VERTEX_BYTES : INITIAL_INDEX_BYTES);
    while (capacity < bytes)
    {
        capacity *= 2;
    }

    device->Destroy(buffers[frame]);
    buffers[frame] = {};
    capacities[frame] = 0;

    rhi::BufferDesc desc;
    desc.Size = capacity;
    desc.Usage = usage;
    desc.Memory = rhi::MemoryUsage::CpuToGpu;
    desc.DebugName = usage == rhi::BufferUsage::Vertex ? "DebugUi vertices" : "DebugUi indices";
    Result<rhi::BufferHandle> buffer = device->CreateBuffer(desc);
    if (!buffer)
    {
        return buffer.GetError();
    }

    buffers[frame] = buffer.Value();
    capacities[frame] = capacity;
    return {};
}

} // namespace hyoshi::debug
