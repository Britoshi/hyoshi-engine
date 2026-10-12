// Instanced sprites: one quad per instance, its four corners given (rotated rectangles and other
// quadrilaterals alike), picked by the vertex index (4-vertex triangle strip), positioned in virtual
// 2D units and mapped to clip space by DrawConstants.
// WebGL 2 builds use a hand-written GLSL ES copy (engine/renderer/src/SpriteGles.h): change both.

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
static const uint IMAGE_WINDOW = 2;
static const uint EDGE_GLOW = 4;

struct SpriteInstance
{
    // Where the texture's top left and top right go, then its bottom left and bottom right.
    [[vk::location(0)]] float4 topCorners;
    [[vk::location(1)]] float4 bottomCorners;
    // u0, v0, u1, v1; with IMAGE_WINDOW, where the texture lies instead (min x, min y, max x,
    // max y in virtual units), the quad showing the part of it under the quad; with EDGE_GLOW, the
    // glow's width (virtual units) and how much of the color is left at the middle.
    [[vk::location(2)]] float4 uvRect;
    [[vk::location(3)]] float4 color;
    [[vk::location(4)]] uint flags;
    // Shown only inside this, in virtual units: min x, min y, max x, max y.
    [[vk::location(5)]] float4 clipRect;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint flags : TEXCOORD1;
    float2 virtualPosition : TEXCOORD2;
    nointerpolation float4 clipRect : TEXCOORD3;
    // EDGE_GLOW: the distance to each side of the triangle (top left, top right, bottom left
    // corners), in virtual units: its barycentric coordinates times its heights.
    float3 edges : TEXCOORD4;
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
    output.uv = (instance.flags & IMAGE_WINDOW) != 0
                    ? (position - instance.uvRect.xy) / (instance.uvRect.zw - instance.uvRect.xy)
                    : lerp(instance.uvRect.xy, instance.uvRect.zw, corner);
    output.edges = float3(0.0, 0.0, 0.0);
    if ((instance.flags & EDGE_GLOW) != 0)
    {
        // A triangle (the bottom corners together): each corner is its height away from the side
        // across from it, and on the other two.
        float2 a = instance.topCorners.xy;
        float2 b = instance.topCorners.zw;
        float2 c = instance.bottomCorners.xy;
        float2 ab = b - a;
        float2 ac = c - a;
        float area = abs((ab.x * ac.y) - (ab.y * ac.x));
        float3 heights = area / max(float3(length(c - b), length(a - c), length(b - a)), float3(1.0e-4));
        output.edges = vertexId == 0 ? float3(heights.x, 0.0, 0.0)
                       : vertexId == 1 ? float3(0.0, heights.y, 0.0)
                                       : float3(0.0, 0.0, heights.z);
        output.uv = instance.uvRect.xy;
    }
    output.color = instance.color;
    output.flags = instance.flags;
    output.virtualPosition = position;
    output.clipRect = instance.clipRect;
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
    // Outside the clip rectangle, nothing (made transparent rather than discarded, which keeps the
    // control flow uniform for the derivatives).
    float2 p = input.virtualPosition;
    float inside = (p.x >= input.clipRect.x && p.y >= input.clipRect.y && p.x <= input.clipRect.z &&
                    p.y <= input.clipRect.w) ? 1.0 : 0.0;
    // The nearest side, and a pixel's worth of it, found for every pixel for the derivative.
    float edge = min(input.edges.x, min(input.edges.y, input.edges.z));
    float edgeWidth = max(fwidth(edge), 1.0e-4);
    if ((input.flags & EDGE_GLOW) != 0)
    {
        // Lit at the edge, fading inward over the glow's width to what's left at the middle; the
        // outer edge smoothed over a pixel.
        float glow = 1.0 - saturate(edge / max(input.uv.x, 1.0e-4));
        float light = lerp(input.uv.y, 1.0, glow * glow) * saturate(edge / edgeWidth);
        return float4(input.color.rgb, input.color.a * light * inside);
    }
    if ((input.flags & DISTANCE_FIELD) != 0)
    {
        // The edge is at 0.5; smooth over about one screen pixel, whatever the scale.
        float coverage = smoothstep(0.5 - width, 0.5 + width, distance);
        return float4(input.color.rgb, input.color.a * coverage * inside);
    }
    float4 shaded = texel * input.color;
    return float4(shaded.rgb, shaded.a * inside);
}
