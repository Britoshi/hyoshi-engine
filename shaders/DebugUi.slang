// Dear ImGui draw lists: textured, vertex-colored triangles in logical pixels.

struct DrawConstants
{
    float2 scale;
    float2 offset;
    float2 rotation; // cos, sin of the swapchain pre-rotation
};

[[vk::push_constant]]
ConstantBuffer<DrawConstants> constants;

[[vk::binding(0, 0)]]
Sampler2D uiTexture;

struct VertexInput
{
    [[vk::location(0)]] float2 position;
    [[vk::location(1)]] float2 uv;
    [[vk::location(2)]] float4 color;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    float2 p = input.position * constants.scale + constants.offset;
    float2 r = constants.rotation;

    VertexOutput output;
    output.position = float4(p.x * r.x - p.y * r.y, p.x * r.y + p.y * r.x, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return input.color * uiTexture.Sample(input.uv);
}
