#include "WebGlRenderDevice.h"

#include "core/Log.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace hyoshi::rhi
{

namespace
{

struct GlAttributeFormat
{
    GLint Size = 1;
    GLenum Type = GL_FLOAT;
    bool IsNormalized = false;
    bool IsInteger = false;
};

GlAttributeFormat ToGlFormat(VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float:
        return {1, GL_FLOAT, false, false};
    case VertexFormat::Float2:
        return {2, GL_FLOAT, false, false};
    case VertexFormat::Float3:
        return {3, GL_FLOAT, false, false};
    case VertexFormat::Float4:
        return {4, GL_FLOAT, false, false};
    case VertexFormat::UByte4Norm:
        return {4, GL_UNSIGNED_BYTE, true, false};
    case VertexFormat::UInt:
        return {1, GL_UNSIGNED_INT, false, true};
    }
    return {};
}

// Compiles one stage; on failure, the compiler's log.
Result<GLuint> CompileShader(GLenum stage, std::string_view source, const char* name)
{
    const GLuint shader = glCreateShader(stage);
    const GLchar* text = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint isCompiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
    if (isCompiled != GL_TRUE)
    {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        return Error{std::string("Couldn't compile the ") + (stage == GL_VERTEX_SHADER ? "vertex" : "fragment") +
                     " shader of " + name + ": " + log};
    }
    return shader;
}

} // namespace

// --- WebGlCommandList ---

void WebGlCommandList::Reset()
{
    pipeline = {};
    vertexBuffers = {};
    indexBuffer = 0;
    indexOffset = 0;
    boundTextures = {};
    boundSamplers = {};
}

void WebGlCommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    const Extent extent = device.surfaceExtent;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Clearing goes through the scissor; the whole target is cleared.
    glDisable(GL_SCISSOR_TEST);
    glClearColor(desc.Clear.R, desc.Clear.G, desc.Clear.B, desc.Clear.A);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    SetViewport({0.0f, 0.0f, static_cast<float>(extent.Width), static_cast<float>(extent.Height)});
    SetScissor({0, 0, extent.Width, extent.Height});
}

void WebGlCommandList::EndRenderPass()
{
}

void WebGlCommandList::BindPipeline(PipelineHandle handle)
{
    const WebGlRenderDevice::PipelineData* data = device.pipelines.Get(handle);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed pipeline");
    pipeline = handle;
    mode = data->Mode;
    if (data->IsBlended)
    {
        glEnable(GL_BLEND);
        // Straight alpha, as the other backends blend.
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        glDisable(GL_BLEND);
    }
}

void WebGlCommandList::BindVertexBuffer(uint32_t binding, BufferHandle buffer, size_t offset)
{
    HYOSHI_ASSERT(binding < MAX_VERTEX_BINDINGS);
    const WebGlRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    vertexBuffers[binding] = {data->Buffer, offset};
}

void WebGlCommandList::BindIndexBuffer(BufferHandle buffer, size_t offset, IndexType type)
{
    const WebGlRenderDevice::BufferData* data = device.buffers.Get(buffer);
    HYOSHI_ASSERT(data != nullptr, "Invalid or destroyed buffer");
    indexBuffer = data->Buffer;
    indexOffset = offset;
    indexType = type == IndexType::UInt16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
}

void WebGlCommandList::BindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler)
{
    HYOSHI_ASSERT(slot < MAX_TEXTURE_SLOTS);
    boundTextures[slot] = texture;
    boundSamplers[slot] = sampler;
}

void WebGlCommandList::PushConstants(const void* data, uint32_t size)
{
    HYOSHI_ASSERT(size <= WebGlRenderDevice::CONSTANT_BUFFER_SIZE);
    // Draws read the buffer as it is when they're issued, so one buffer serves every push.
    glBindBuffer(GL_UNIFORM_BUFFER, device.constantBuffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(size), data);
}

void WebGlCommandList::SetViewport(const Viewport& viewport)
{
    const auto height = static_cast<float>(device.surfaceExtent.Height);
    glViewport(static_cast<GLint>(std::lround(viewport.X)),
               static_cast<GLint>(std::lround(height - viewport.Y - viewport.Height)),
               static_cast<GLsizei>(std::lround(viewport.Width)), static_cast<GLsizei>(std::lround(viewport.Height)));
}

void WebGlCommandList::SetScissor(const Rect& scissor)
{
    const Extent extent = device.surfaceExtent;
    const auto x0 = std::clamp<int64_t>(scissor.X, 0, extent.Width);
    const auto y0 = std::clamp<int64_t>(scissor.Y, 0, extent.Height);
    const auto x1 = std::clamp<int64_t>(static_cast<int64_t>(scissor.X) + scissor.Width, 0, extent.Width);
    const auto y1 = std::clamp<int64_t>(static_cast<int64_t>(scissor.Y) + scissor.Height, 0, extent.Height);
    glScissor(static_cast<GLint>(x0), static_cast<GLint>(extent.Height - y1), static_cast<GLsizei>(x1 - x0),
              static_cast<GLsizei>(y1 - y0));
}

bool WebGlCommandList::PrepareDraw(int64_t baseVertex, uint32_t firstInstance)
{
    const WebGlRenderDevice::PipelineData* data = device.pipelines.Get(pipeline);
    if (data == nullptr)
    {
        HYOSHI_LOG_ERROR("WebGL: a draw with no pipeline bound");
        return false;
    }
    if (glProgram != data->Program)
    {
        glUseProgram(data->Program);
        glProgram = data->Program;
    }

    uint32_t used = 0;
    for (const WebGlRenderDevice::AttributeLayout& layout : data->Attributes)
    {
        const VertexBufferBinding& binding = vertexBuffers[layout.Binding];
        if (binding.Buffer == 0)
        {
            HYOSHI_LOG_ERROR("WebGL: vertex buffer {} isn't bound", layout.Binding);
            return false;
        }
        const int64_t first = layout.IsPerInstance ? static_cast<int64_t>(firstInstance) : baseVertex;
        const int64_t pointer = static_cast<int64_t>(binding.Offset) + layout.Offset + (first * layout.Stride);
        AttributeState& state = attributes[layout.Location];
        if (!state.IsEnabled)
        {
            glEnableVertexAttribArray(layout.Location);
            state.IsEnabled = true;
        }
        if (state.Buffer != binding.Buffer || state.Pointer != pointer || state.Stride != layout.Stride ||
            state.Format != layout.Format)
        {
            const GlAttributeFormat format = ToGlFormat(layout.Format);
            glBindBuffer(GL_ARRAY_BUFFER, binding.Buffer);
            // NOLINTNEXTLINE(performance-no-int-to-ptr): GL takes buffer offsets as pointers.
            const auto* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(std::max<int64_t>(pointer, 0)));
            if (format.IsInteger)
            {
                glVertexAttribIPointer(layout.Location, format.Size, format.Type, static_cast<GLsizei>(layout.Stride),
                                       offset);
            }
            else
            {
                glVertexAttribPointer(layout.Location, format.Size, format.Type,
                                      format.IsNormalized ? GL_TRUE : GL_FALSE, static_cast<GLsizei>(layout.Stride),
                                      offset);
            }
            state.Buffer = binding.Buffer;
            state.Pointer = pointer;
            state.Stride = layout.Stride;
            state.Format = layout.Format;
        }
        const uint32_t divisor = layout.IsPerInstance ? 1 : 0;
        if (state.Divisor != divisor)
        {
            glVertexAttribDivisor(layout.Location, divisor);
            state.Divisor = divisor;
        }
        used |= 1u << layout.Location;
    }
    // Attributes another pipeline left on would need buffers of their own.
    for (uint32_t location = 0; location < MAX_ATTRIBUTES; ++location)
    {
        if (attributes[location].IsEnabled && (used & (1u << location)) == 0)
        {
            glDisableVertexAttribArray(location);
            attributes[location].IsEnabled = false;
        }
    }

    for (uint32_t slot = 0; slot < data->TextureCount; ++slot)
    {
        const WebGlRenderDevice::TextureData* texture = device.textures.Get(boundTextures[slot]);
        const WebGlRenderDevice::SamplerData* sampler = device.samplers.Get(boundSamplers[slot]);
        if (texture == nullptr || sampler == nullptr)
        {
            HYOSHI_LOG_ERROR("WebGL: texture slot {} is unbound or destroyed", slot);
            return false;
        }
        if (glTextures[slot] != texture->Texture)
        {
            glActiveTexture(GL_TEXTURE0 + slot);
            glBindTexture(GL_TEXTURE_2D, texture->Texture);
            glTextures[slot] = texture->Texture;
        }
        if (glSamplers[slot] != sampler->Sampler)
        {
            glBindSampler(slot, sampler->Sampler);
            glSamplers[slot] = sampler->Sampler;
        }
    }
    return true;
}

void WebGlCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
    if (!PrepareDraw(0, firstInstance))
    {
        return;
    }
    glDrawArraysInstanced(mode, static_cast<GLint>(firstVertex), static_cast<GLsizei>(vertexCount),
                          static_cast<GLsizei>(instanceCount));
}

void WebGlCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                   int32_t vertexOffset, uint32_t firstInstance)
{
    if (indexBuffer == 0)
    {
        HYOSHI_LOG_ERROR("WebGL: an indexed draw with no index buffer bound");
        return;
    }
    if (!PrepareDraw(vertexOffset, firstInstance))
    {
        return;
    }
    if (glIndexBuffer != indexBuffer)
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glIndexBuffer = indexBuffer;
    }
    const size_t indexSize = indexType == GL_UNSIGNED_SHORT ? 2 : 4;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): GL takes buffer offsets as pointers.
    const auto* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(indexOffset + (firstIndex * indexSize)));
    glDrawElementsInstanced(mode, static_cast<GLsizei>(indexCount), indexType, offset,
                            static_cast<GLsizei>(instanceCount));
}

// --- WebGlRenderDevice: setup and teardown ---

WebGlRenderDevice::WebGlRenderDevice() : commandList(*this)
{
}

WebGlRenderDevice::~WebGlRenderDevice()
{
    Shutdown();
}

Result<void> WebGlRenderDevice::Initialize(const DeviceConfig& /*config*/, platform::Platform& targetPlatform)
{
    HYOSHI_ASSERT(!isInitialized);
    platform = &targetPlatform;

    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    // Opaque, as the other backends present; nothing needs depth, stencil, or multisampling.
    attributes.alpha = EM_FALSE;
    attributes.depth = EM_FALSE;
    attributes.stencil = EM_FALSE;
    attributes.antialias = EM_FALSE;
    attributes.preserveDrawingBuffer = EM_FALSE;
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    context = emscripten_webgl_create_context("#canvas", &attributes);
    if (context <= 0)
    {
        return Error{"Could not create a WebGL 2 context on #canvas"};
    }
    emscripten_webgl_make_context_current(context);

    // One vertex array, its attributes pointed anew as pipelines and buffers change.
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glGenBuffers(1, &constantBuffer);
    glBindBuffer(GL_UNIFORM_BUFFER, constantBuffer);
    glBufferData(GL_UNIFORM_BUFFER, CONSTANT_BUFFER_SIZE, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, constantBuffer);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    hasSurface = true;
    surfaceExtent = ReadDrawingBufferSize();
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    HYOSHI_LOG_INFO("WebGL 2 (no WebGPU here): {}", renderer != nullptr ? renderer : "unknown renderer");
    isInitialized = true;
    return {};
}

void WebGlRenderDevice::Shutdown()
{
    if (context <= 0)
    {
        return;
    }
    emscripten_webgl_make_context_current(context);
    pipelines.ForEach([](PipelineData& data) { glDeleteProgram(data.Program); });
    pipelines.Clear();
    samplers.ForEach([](SamplerData& data) { glDeleteSamplers(1, &data.Sampler); });
    samplers.Clear();
    textures.ForEach([](TextureData& data) { glDeleteTextures(1, &data.Texture); });
    textures.Clear();
    buffers.ForEach([](BufferData& data) { glDeleteBuffers(1, &data.Buffer); });
    buffers.Clear();
    glDeleteBuffers(1, &constantBuffer);
    constantBuffer = 0;
    glDeleteVertexArrays(1, &vertexArray);
    vertexArray = 0;
    emscripten_webgl_destroy_context(context);
    context = 0;
    hasSurface = false;
    isInitialized = false;
}

// --- Surface: the canvas's drawing buffer, which follows the canvas's size ---

Result<void> WebGlRenderDevice::CreateSurface()
{
    hasSurface = context > 0;
    surfaceExtent = ReadDrawingBufferSize();
    return {};
}

void WebGlRenderDevice::DestroySurface()
{
    hasSurface = false;
    surfaceExtent = {};
}

bool WebGlRenderDevice::HasSurface() const
{
    return hasSurface;
}

Result<void> WebGlRenderDevice::RecreateSwapchain()
{
    surfaceExtent = ReadDrawingBufferSize();
    return {};
}

Extent WebGlRenderDevice::ReadDrawingBufferSize() const
{
    int width = 0;
    int height = 0;
    if (context > 0)
    {
        emscripten_webgl_get_drawing_buffer_size(context, &width, &height);
    }
    return {static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))};
}

// --- Resources ---

Result<BufferHandle> WebGlRenderDevice::CreateBuffer(const BufferDesc& desc)
{
    BufferData data;
    data.Size = desc.Size;
    data.IsIndex = desc.Usage == BufferUsage::Index;
    glGenBuffers(1, &data.Buffer);
    const GLenum usage = desc.Memory == MemoryUsage::CpuToGpu ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    if (data.IsIndex)
    {
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, data.Buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(desc.Size), nullptr, usage);
        glBindVertexArray(vertexArray);
    }
    else
    {
        glBindBuffer(GL_ARRAY_BUFFER, data.Buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(desc.Size), nullptr, usage);
    }
    return buffers.Create(data);
}

Result<TextureHandle> WebGlRenderDevice::CreateTexture(const TextureDesc& desc)
{
    TextureData data;
    data.Width = desc.Width;
    data.Height = desc.Height;
    data.IsExpandedR8 = desc.Format == TextureFormat::R8Unorm;
    glGenTextures(1, &data.Texture);
    glActiveTexture(UPLOAD_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, data.Texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(desc.Width), static_cast<GLsizei>(desc.Height));
    // Samplers set filtering; this keeps the texture complete without them.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return textures.Create(data);
}

Result<SamplerHandle> WebGlRenderDevice::CreateSampler(const SamplerDesc& desc)
{
    SamplerData data;
    glGenSamplers(1, &data.Sampler);
    const GLint filter = desc.MinMagFilter == Filter::Nearest ? GL_NEAREST : GL_LINEAR;
    const GLint address = desc.Address == AddressMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glSamplerParameteri(data.Sampler, GL_TEXTURE_MIN_FILTER, filter);
    glSamplerParameteri(data.Sampler, GL_TEXTURE_MAG_FILTER, filter);
    glSamplerParameteri(data.Sampler, GL_TEXTURE_WRAP_S, address);
    glSamplerParameteri(data.Sampler, GL_TEXTURE_WRAP_T, address);
    return samplers.Create(data);
}

void WebGlRenderDevice::Destroy(BufferHandle buffer)
{
    // GL keeps what queued draws use alive, so deleting now is safe.
    if (BufferData* data = buffers.Get(buffer))
    {
        for (WebGlCommandList::AttributeState& state : commandList.attributes)
        {
            if (state.Buffer == data->Buffer)
            {
                state = {.IsEnabled = state.IsEnabled};
            }
        }
        if (commandList.glIndexBuffer == data->Buffer)
        {
            commandList.glIndexBuffer = 0;
        }
        glDeleteBuffers(1, &data->Buffer);
        buffers.Destroy(buffer);
    }
}

void WebGlRenderDevice::Destroy(TextureHandle texture)
{
    if (TextureData* data = textures.Get(texture))
    {
        for (GLuint& bound : commandList.glTextures)
        {
            bound = bound == data->Texture ? 0 : bound;
        }
        glDeleteTextures(1, &data->Texture);
        textures.Destroy(texture);
    }
}

void WebGlRenderDevice::Destroy(SamplerHandle sampler)
{
    if (SamplerData* data = samplers.Get(sampler))
    {
        for (GLuint& bound : commandList.glSamplers)
        {
            bound = bound == data->Sampler ? 0 : bound;
        }
        glDeleteSamplers(1, &data->Sampler);
        samplers.Destroy(sampler);
    }
}

void WebGlRenderDevice::Destroy(PipelineHandle pipeline)
{
    if (PipelineData* data = pipelines.Get(pipeline))
    {
        if (commandList.glProgram == data->Program)
        {
            commandList.glProgram = 0;
        }
        glDeleteProgram(data->Program);
        pipelines.Destroy(pipeline);
    }
}

Result<void> WebGlRenderDevice::UploadBuffer(BufferHandle buffer, const void* data, size_t size, size_t offset)
{
    const BufferData* bufferData = buffers.Get(buffer);
    if (bufferData == nullptr)
    {
        return Error{"UploadBuffer: invalid or destroyed buffer"};
    }
    if (size == 0)
    {
        return {};
    }
    if (offset + size > bufferData->Size)
    {
        return Error{"UploadBuffer: the range is past the end of the buffer"};
    }
    if (bufferData->IsIndex)
    {
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufferData->Buffer);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
        glBindVertexArray(vertexArray);
    }
    else
    {
        glBindBuffer(GL_ARRAY_BUFFER, bufferData->Buffer);
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    }
    return {};
}

Result<void> WebGlRenderDevice::UpdateTexture(TextureHandle texture, const void* pixels, uint32_t x, uint32_t y,
                                              uint32_t width, uint32_t height)
{
    const TextureData* data = textures.Get(texture);
    if (data == nullptr)
    {
        return Error{"UpdateTexture: invalid or destroyed texture"};
    }
    if (width == 0 || height == 0)
    {
        return {};
    }

    std::vector<uint8_t> expanded;
    if (data->IsExpandedR8)
    {
        const size_t count = static_cast<size_t>(width) * height;
        const auto* values = static_cast<const uint8_t*>(pixels);
        expanded.resize(count * 4);
        for (size_t index = 0; index < count; ++index)
        {
            expanded[(index * 4) + 0] = 255;
            expanded[(index * 4) + 1] = 255;
            expanded[(index * 4) + 2] = 255;
            expanded[(index * 4) + 3] = values[index];
        }
        pixels = expanded.data();
    }
    glActiveTexture(UPLOAD_TEXTURE_UNIT);
    glBindTexture(GL_TEXTURE_2D, data->Texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return {};
}

// --- Pipelines ---

Result<PipelineHandle> WebGlRenderDevice::CreatePipeline(const PipelineDesc& desc)
{
    // 3D's settings (decision 0003), which only Vulkan has so far.
    if (desc.Depth != DepthMode::None || desc.Cull != CullMode::None || desc.Blend == BlendMode::Additive)
    {
        return Error{std::string("No depth, culling, or additive blending on WebGL 2 yet (") + desc.DebugName + ")"};
    }
    if (desc.GlesVertex.empty() || desc.GlesFragment.empty())
    {
        return Error{std::string("No GLSL ES for pipeline ") + desc.DebugName};
    }
    Result<GLuint> vertex = CompileShader(GL_VERTEX_SHADER, desc.GlesVertex, desc.DebugName);
    if (!vertex)
    {
        return vertex.GetError();
    }
    Result<GLuint> fragment = CompileShader(GL_FRAGMENT_SHADER, desc.GlesFragment, desc.DebugName);
    if (!fragment)
    {
        glDeleteShader(vertex.Value());
        return fragment.GetError();
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex.Value());
    glAttachShader(program, fragment.Value());
    glLinkProgram(program);
    glDeleteShader(vertex.Value());
    glDeleteShader(fragment.Value());
    GLint isLinked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
    if (isLinked != GL_TRUE)
    {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        glDeleteProgram(program);
        return Error{std::string("Couldn't link pipeline ") + desc.DebugName + ": " + log};
    }

    // The constants at uniform buffer binding 0, texture slot N at texture unit N.
    if (const GLuint block = glGetUniformBlockIndex(program, "Constants"); block != GL_INVALID_INDEX)
    {
        glUniformBlockBinding(program, block, 0);
    }
    glUseProgram(program);
    commandList.glProgram = program;
    for (uint32_t slot = 0; slot < desc.TextureCount; ++slot)
    {
        const std::string name = "texture" + std::to_string(slot);
        if (const GLint location = glGetUniformLocation(program, name.c_str()); location >= 0)
        {
            glUniform1i(location, static_cast<GLint>(slot));
        }
    }

    PipelineData data;
    data.Program = program;
    data.Mode = desc.Topology == PrimitiveTopology::TriangleStrip ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
    data.IsBlended = desc.Blend == BlendMode::Alpha;
    data.TextureCount = desc.TextureCount;
    data.HasConstants = desc.PushConstantSize > 0;
    for (const VertexAttributeDesc& attribute : desc.VertexAttributes)
    {
        const auto binding = std::find_if(desc.VertexBindings.begin(), desc.VertexBindings.end(),
                                          [&attribute](const VertexBindingDesc& candidate)
                                          { return candidate.Binding == attribute.Binding; });
        if (binding == desc.VertexBindings.end() || attribute.Location >= WebGlCommandList::MAX_ATTRIBUTES ||
            attribute.Binding >= WebGlCommandList::MAX_VERTEX_BINDINGS)
        {
            glDeleteProgram(program);
            return Error{std::string("Pipeline ") + desc.DebugName + " has an attribute WebGL can't take"};
        }
        data.Attributes.push_back({attribute.Location, attribute.Binding, attribute.Format, attribute.Offset,
                                   binding->Stride, binding->InputRate == VertexInputRate::Instance});
    }
    return pipelines.Create(std::move(data));
}

// --- Frames ---

FrameStatus WebGlRenderDevice::BeginFrame()
{
    HYOSHI_ASSERT(!isFrameActive, "BeginFrame called twice");
    if (!hasSurface)
    {
        return FrameStatus::Skipped;
    }
    surfaceExtent = ReadDrawingBufferSize();
    if (surfaceExtent.Width == 0 || surfaceExtent.Height == 0)
    {
        return FrameStatus::Skipped;
    }
    emscripten_webgl_make_context_current(context);
    commandList.Reset();
    isFrameActive = true;
    return FrameStatus::Ready;
}

ICommandList& WebGlRenderDevice::GetCommandList()
{
    HYOSHI_ASSERT(isFrameActive, "GetCommandList is only valid between BeginFrame and EndFrameAndPresent");
    return commandList;
}

void WebGlRenderDevice::EndFrameAndPresent()
{
    HYOSHI_ASSERT(isFrameActive);
#if !defined(NDEBUG)
    // Debug builds count GL errors (each check waits on the GPU process, so release builds don't).
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError())
    {
        ++errorCount;
        HYOSHI_LOG_WARN("WebGL error 0x{:x}", error);
    }
#endif
    // The browser presents the canvas when control returns to it; there's no present call.
    isFrameActive = false;
    frameIndex = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
}

uint32_t WebGlRenderDevice::GetFrameIndex() const
{
    return frameIndex;
}

Extent WebGlRenderDevice::GetSwapchainExtent() const
{
    return surfaceExtent;
}

SurfaceTransform WebGlRenderDevice::GetSurfaceTransform() const
{
    return SurfaceTransform::Identity;
}

void WebGlRenderDevice::SavePipelineCache()
{
}

uint32_t WebGlRenderDevice::GetValidationMessageCount() const
{
    return errorCount;
}

void WebGlRenderDevice::RequestScreenshot(std::string path)
{
    HYOSHI_LOG_WARN("Screenshots aren't supported on the web ('{}')", path);
}

} // namespace hyoshi::rhi
