// Instanced sprites: one quad per instance, its four corners given (rotated rectangles and other
// quadrilaterals alike), picked by the vertex index (4-vertex triangle strip), positioned in virtual
// 2D units and mapped to clip space by DrawConstants.

struct DrawConstants
{
    float2 scale;
    float2 offset;
    float2 rotation; // cos, sin of the swapchain pre-rotation
};

// WebGPU has no push constants: web builds read them from a uniform buffer (engine/rhi/webgpu).
#if defined(HYOSHI_WGSL)
[[vk::binding(0, 1)]]
#else
[[vk::push_constant]]
#endif
ConstantBuffer<DrawConstants> constants;

[[vk::binding(0, 0)]]
Sampler2D spriteTexture;

// Matches the flags in SpriteBatch::Instance.
static const uint DISTANCE_FIELD = 1;

struct SpriteInstance
{
    // Where the texture's top left and top right go, then its bottom left and bottom right.
    [[vk::location(0)]] float4 topCorners;
    [[vk::location(1)]] float4 bottomCorners;
    [[vk::location(2)]] float4 uvRect;
    [[vk::location(3)]] float4 color;
    [[vk::location(4)]] uint flags;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint flags : TEXCOORD1;
};

float2 ToClip(float2 position)
{
    float2 p = position * constants.scale + constants.offset;
    float2 r = constants.rotation;
    return float2(p.x * r.x - p.y * r.y, p.x * r.y + p.y * r.x);
}

[shader("vertex")]
VertexOutput vertexMain(SpriteInstance instance, uint vertexId: SV_VulkanVertexID)
{
    float2 corner = float2(float(vertexId & 1), float(vertexId >> 1));
    float4 edge = (vertexId >> 1) != 0 ? instance.bottomCorners : instance.topCorners;
    float2 position = (vertexId & 1) != 0 ? edge.zw : edge.xy;

    VertexOutput output;
    output.position = float4(ToClip(position), 0.0, 1.0);
#if defined(HYOSHI_WGSL)
    // Clip space y points up in WebGPU, down in Vulkan.
    output.position.y = -output.position.y;
#endif
    output.uv = lerp(instance.uvRect.xy, instance.uvRect.zw, corner);
    output.color = instance.color;
    output.flags = instance.flags;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float4 texel = spriteTexture.Sample(input.uv);
    // Derivatives are only defined in uniform control flow (WGSL rejects them in the branch), so
    // the edge width is found for every pixel, distance field or not.
    float distance = texel.a;
    float width = max(fwidth(distance) * 0.7, 1.0e-4);
    if ((input.flags & DISTANCE_FIELD) != 0)
    {
        // The edge is at 0.5; smooth over about one screen pixel, whatever the scale.
        float coverage = smoothstep(0.5 - width, 0.5 + width, distance);
        return float4(input.color.rgb, input.color.a * coverage);
    }
    return texel * input.color;
}
