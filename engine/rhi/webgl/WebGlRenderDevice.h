#pragma once

#include "core/Handle.h"
#include "rhi/IRenderDevice.h"

#include <GLES3/gl3.h>
#include <emscripten/html5.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hyoshi::rhi
{

class WebGlRenderDevice;

class WebGlCommandList final : public ICommandList
{
public:
    explicit WebGlCommandList(WebGlRenderDevice& owner) : device(owner)
    {
    }

    void Reset();

    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;
    void BindPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset) override;
    void BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type) override;
    void BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
    // None here: storage buffers are read by 3D's skinned meshes, and the web has no 3D yet.
    void BindStorageBuffer(uint32_t /*slot*/, BufferHandle /*buffer*/, size_t /*offset*/, size_t /*size*/) override
    {
    }
    void PushConstants(const void* data, uint32_t size) override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const Rect& scissor) override;
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance) override;

    static constexpr uint32_t MAX_TEXTURE_SLOTS = 4;
    static constexpr uint32_t MAX_VERTEX_BINDINGS = 4;
    static constexpr uint32_t MAX_ATTRIBUTES = 16;

private:
    friend class WebGlRenderDevice;

    struct VertexBufferBinding
    {
        GLuint Buffer = 0;
        size_t Offset = 0;
    };
    // What a vertex attribute is set to in GL now, so calls that change nothing are skipped.
    struct AttributeState
    {
        GLuint Buffer = 0;
        int64_t Pointer = -1;
        uint32_t Stride = 0;
        VertexFormat Format = VertexFormat::Float;
        uint32_t Divisor = UINT32_MAX;
        bool IsEnabled = false;
    };

    // Points the pipeline's attributes at the bound buffers, the vertex-rate ones `baseVertex`
    // vertices in and the instance-rate ones `firstInstance` instances in (WebGL 2 has neither a
    // base vertex nor a base instance), and binds its textures. False if something is unbound.
    bool PrepareDraw(int64_t baseVertex, uint32_t firstInstance);

    WebGlRenderDevice& device;
    PipelineHandle pipeline;
    GLenum mode = GL_TRIANGLES;
    std::array<VertexBufferBinding, MAX_VERTEX_BINDINGS> vertexBuffers{};
    GLuint indexBuffer = 0;
    size_t indexOffset = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    std::array<TextureHandle, MAX_TEXTURE_SLOTS> boundTextures{};
    std::array<SamplerHandle, MAX_TEXTURE_SLOTS> boundSamplers{};

    std::array<AttributeState, MAX_ATTRIBUTES> attributes{};
    std::array<GLuint, MAX_TEXTURE_SLOTS> glTextures{};
    std::array<GLuint, MAX_TEXTURE_SLOTS> glSamplers{};
    GLuint glIndexBuffer = 0;
    GLuint glProgram = 0;
};

// WebGL 2, for browsers without WebGPU (an older Chromium, as in cars): through Emscripten's
// OpenGL ES 3.0 on the page's #canvas. Pipelines are GLSL ES 3.00 programs (PipelineDesc::Gles*)
// whose push constants are a std140 uniform block named Constants, filled from one uniform
// buffer, and whose texture slot N is the sampler2D textureN. R8 textures are stored as RGBA8,
// white with the value in alpha, as Vulkan reads them through a swizzle (WebGL has none). Frame
// buffer rows run bottom up, so viewports and scissors are flipped.
class WebGlRenderDevice final : public IRenderDevice
{
public:
    WebGlRenderDevice();
    ~WebGlRenderDevice() override;

    WebGlRenderDevice(const WebGlRenderDevice&) = delete;
    WebGlRenderDevice& operator=(const WebGlRenderDevice&) = delete;

    Result<void> Initialize(const DeviceConfig& config, platform::Platform& platform) override;
    void Shutdown() override;

    Result<void> CreateSurface() override;
    void DestroySurface() override;
    bool HasSurface() const override;
    Result<void> RecreateSwapchain() override;

    Result<BufferHandle> CreateBuffer(const BufferDesc& desc) override;
    Result<TextureHandle> CreateTexture(const TextureDesc& desc) override;
    Result<SamplerHandle> CreateSampler(const SamplerDesc& desc) override;
    Result<PipelineHandle> CreatePipeline(const PipelineDesc& desc) override;
    void Destroy(BufferHandle buffer) override;
    void Destroy(TextureHandle texture) override;
    void Destroy(SamplerHandle sampler) override;
    void Destroy(PipelineHandle pipeline) override;

    Result<void> UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset) override;
    Result<void> UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height) override;
    // No mipmaps on the web yet: every texture keeps one level (TextureDesc::MipLevels).
    Result<void> UpdateTextureLevel(TextureHandle /*texture*/, uint32_t /*level*/, const void* /*pixels*/) override
    {
        return {};
    }

    FrameStatus BeginFrame() override;
    ICommandList& GetCommandList() override;
    void EndFrameAndPresent() override;

    uint32_t GetFrameIndex() const override;
    Extent GetSwapchainExtent() const override;
    SurfaceTransform GetSurfaceTransform() const override;

    void SavePipelineCache() override;
    uint32_t GetValidationMessageCount() const override;
    bool HasDepth() const override
    {
        return false;
    }
    void RequestScreenshot(std::string path) override;

private:
    friend class WebGlCommandList;

    struct BufferData
    {
        GLuint Buffer = 0;
        size_t Size = 0;
        // Index buffers are only ever bound as such (WebGL keeps the two kinds apart), so they're
        // filled with the vertex array unbound, leaving the game's index binding alone.
        bool IsIndex = false;
    };

    struct TextureData
    {
        GLuint Texture = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool IsExpandedR8 = false;
    };

    struct SamplerData
    {
        GLuint Sampler = 0;
    };

    // An attribute with its binding's stride and rate.
    struct AttributeLayout
    {
        uint32_t Location = 0;
        uint32_t Binding = 0;
        VertexFormat Format = VertexFormat::Float;
        uint32_t Offset = 0;
        uint32_t Stride = 0;
        bool IsPerInstance = false;
    };

    struct PipelineData
    {
        GLuint Program = 0;
        std::vector<AttributeLayout> Attributes;
        GLenum Mode = GL_TRIANGLES;
        bool IsBlended = false;
        uint32_t TextureCount = 0;
        bool HasConstants = false;
    };

    // Push constants: at most this many bytes, in one uniform buffer at binding 0.
    static constexpr GLsizeiptr CONSTANT_BUFFER_SIZE = 256;
    // Uploads go through this texture unit, which draws never use, so they disturb nothing bound.
    static constexpr GLenum UPLOAD_TEXTURE_UNIT = GL_TEXTURE0 + 8;

    Extent ReadDrawingBufferSize() const;

    platform::Platform* platform = nullptr;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = 0;
    GLuint vertexArray = 0;
    GLuint constantBuffer = 0;
    Extent surfaceExtent;
    bool hasSurface = false;
    uint32_t frameIndex = 0;
    bool isFrameActive = false;
    uint32_t errorCount = 0;

    WebGlCommandList commandList;
    HandlePool<BufferData, BufferTag> buffers;
    HandlePool<TextureData, TextureTag> textures;
    HandlePool<SamplerData, SamplerTag> samplers;
    HandlePool<PipelineData, PipelineTag> pipelines;

    bool isInitialized = false;
};

} // namespace hyoshi::rhi
