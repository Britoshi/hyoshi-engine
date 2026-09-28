// Instanced sprites: one quad per instance, corners generated from the vertex index (4-vertex
// triangle strip), positioned in virtual 2D units and mapped to clip space by DrawConstants.

struct DrawConstants
{
    float2 scale;
    float2 offset;
    float2 rotation; // cos, sin of the swapchain pre-rotation
};

[[vk::push_constant]]
ConstantBuffer<DrawConstants> constants;

[[vk::binding(0, 0)]]
Sampler2D spriteTexture;

// Matches the flags in SpriteBatch::Instance.
static const uint DISTANCE_FIELD = 1;

struct SpriteInstance
{
    [[vk::location(0)]] float2 center;
    [[vk::location(1)]] float2 size;
    [[vk::location(2)]] float angle;
    [[vk::location(3)]] float4 uvRect;
    [[vk::location(4)]] float4 color;
    [[vk::location(5)]] uint flags;
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
    float2 local = (corner - 0.5) * instance.size;

    float s = sin(instance.angle);
    float c = cos(instance.angle);
    float2 rotated = float2(local.x * c - local.y * s, local.x * s + local.y * c);

    VertexOutput output;
    output.position = float4(ToClip(instance.center + rotated), 0.0, 1.0);
    output.uv = lerp(instance.uvRect.xy, instance.uvRect.zw, corner);
    output.color = instance.color;
    output.flags = instance.flags;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float4 texel = spriteTexture.Sample(input.uv);
    if ((input.flags & DISTANCE_FIELD) != 0)
    {
        // The edge is at 0.5; smooth over about one screen pixel, whatever the scale.
        float distance = texel.a;
        float width = max(fwidth(distance) * 0.7, 1.0e-4);
        float coverage = smoothstep(0.5 - width, 0.5 + width, distance);
        return float4(input.color.rgb, input.color.a * coverage);
    }
    return texel * input.color;
}
