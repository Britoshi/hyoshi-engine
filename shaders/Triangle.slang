// M1 bring-up shader: a single colored triangle generated from the vertex index, no buffers.

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR0;
};

static const float2 POSITIONS[3] = { float2(0.0, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6) };
static const float3 COLORS[3] = { float3(1.0, 0.25, 0.45), float3(0.2, 0.8, 1.0), float3(1.0, 0.85, 0.2) };

[shader("vertex")]
VertexOutput vertexMain(uint vertexId: SV_VulkanVertexID)
{
    VertexOutput output;
    output.position = float4(POSITIONS[vertexId], 0.0, 1.0);
    output.color = COLORS[vertexId];
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
