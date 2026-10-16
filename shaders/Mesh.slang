// 3D meshes (decision 0003): lit by one directional light, an ambient that runs from the ground's
// color below to the sky's above, and a rim at the silhouette, or glowing in their own color
// (emission); smoothly or as a cartoon is (toon, from the light's alpha); fogged with distance. Colors arrive as sRGB, as everywhere in the engine; lighting is
// done in linear light and written back as sRGB, since the swapchain takes colors as they are.
// Everything per draw fits the 128 bytes of push constants every device has (Renderer3D.cpp).

struct MeshConstants
{
    // Object space to clip space, by columns.
    float4 clip0;
    float4 clip1;
    float4 clip2;
    float4 clip3;
    // Toward the light, in object space; w: emission (0 lit, 1 only its own color).
    float4 lightDirection;
    // The camera, in object space; w: the rim's strength.
    float4 cameraPosition;
    // The world's up, in object space; w: the base color's RGBA8 bits.
    float4 up;
    // RGBA8 each: the light (its alpha how toon the surface is), the sky, the ground, and the fog
    // (its alpha the density).
    uint4 colors;
};

// WebGPU has no push constants: web builds read them from a uniform buffer (engine/rhi/webgpu).
#if defined(HYOSHI_WGSL)
[[vk::binding(0, 1)]]
#else
[[vk::push_constant]]
#endif
ConstantBuffer<MeshConstants> constants;

[[vk::binding(0, 0)]]
Sampler2D baseTexture;

// Fog's density per unit of distance at an alpha of 255.
static const float FOG_DENSITY_SCALE = 1.0;
// Toon: where the light's side ends (the cosine toward the light) and how soft that edge is; the
// shadow's tint (a little cool and pink, as cartoons shade skin and cloth); the rim's band.
static const float TOON_EDGE = 0.12;
static const float TOON_SOFTNESS = 0.04;
static const float3 TOON_SHADE = float3(0.78, 0.7, 0.9);
static const float TOON_RIM_EDGE = 0.62;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 color;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 objectPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 color : COLOR0;
    // The distance along the view, for fog (clip space's w).
    float viewDepth : TEXCOORD3;
};

float4 Unpack(uint packed)
{
    return float4(float(packed & 255u), float((packed >> 8u) & 255u), float((packed >> 16u) & 255u),
                  float(packed >> 24u)) /
           255.0;
}

float3 ToLinear(float3 color)
{
    return pow(max(color, float3(0.0)), float3(2.2));
}

float3 ToSrgb(float3 color)
{
    return pow(max(color, float3(0.0)), float3(1.0 / 2.2));
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = constants.clip0 * input.position.x + constants.clip1 * input.position.y +
                      constants.clip2 * input.position.z + constants.clip3;
    output.objectPosition = input.position;
    output.normal = input.normal;
    output.uv = input.uv;
    output.color = input.color;
    output.viewDepth = output.position.w;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool isFrontFace: SV_IsFrontFace) : SV_Target
{
    float4 base = Unpack(asuint(constants.up.w)) * input.color * baseTexture.Sample(input.uv);
    float3 albedo = ToLinear(base.rgb);

    // A two-sided surface seen from behind is lit as its back.
    float3 normal = normalize(input.normal) * (isFrontFace ? 1.0 : -1.0);
    float3 toLight = normalize(constants.lightDirection.xyz);
    float3 toCamera = normalize(constants.cameraPosition.xyz - input.objectPosition);
    float3 light = ToLinear(Unpack(constants.colors.x).rgb);
    float3 sky = ToLinear(Unpack(constants.colors.y).rgb);
    float3 ground = ToLinear(Unpack(constants.colors.z).rgb);
    float4 fog = Unpack(constants.colors.w);

    float toon = Unpack(constants.colors.x).a;
    float3 ambient = lerp(ground, sky, dot(normal, normalize(constants.up.xyz)) * 0.5 + 0.5);
    float facing = dot(normal, toLight);
    float band = smoothstep(TOON_EDGE - TOON_SOFTNESS, TOON_EDGE + TOON_SOFTNESS, facing);
    float diffuse = lerp(max(facing, 0.0), band, toon);
    float3 shade = lerp(float3(1.0), TOON_SHADE, toon * (1.0 - band));
    float edge = 1.0 - saturate(dot(normal, toCamera));
    float rimBand = smoothstep(TOON_RIM_EDGE - TOON_SOFTNESS, TOON_RIM_EDGE + TOON_SOFTNESS, edge) * 0.5;
    float rim = constants.cameraPosition.w * lerp(pow(edge, 3.0), rimBand, toon);
    float3 lit = albedo * (ambient * shade + light * diffuse) + light * rim;
    float3 color = lerp(lit, albedo, constants.lightDirection.w);

    float fogAmount = 1.0 - exp(-input.viewDepth * fog.a * FOG_DENSITY_SCALE);
    color = lerp(color, ToLinear(fog.rgb), fogAmount);
    return float4(ToSrgb(color), base.a);
}
