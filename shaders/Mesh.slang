// 3D meshes (decision 0003): lit by one directional light, an ambient that runs from the ground's
// color below to the sky's above, and a rim at the silhouette, or glowing in their own color
// (emission); smoothly or as a cartoon is (toon, from the light's alpha); fogged with distance. Colors arrive as sRGB, as everywhere in the engine; lighting is
// done in linear light and written back as sRGB, since the swapchain takes colors as they are.
// Everything per draw fits the 128 bytes of push constants every device has (Renderer3D.cpp).
// A skinned mesh (vertexSkinned) is moved by its joints here, on the GPU: the CPU sends the joints
// each frame, not the vertices. Lights at points (a candle, a fire) add to the light, and the
// lighting's grade comes last (pointLights). Where light was baked (lightmaps: ray traced offline),
// it lights the surface instead, for a texture's look-up or two.

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

// Not on the web, which has no 3D yet.
#if !defined(HYOSHI_WGSL)
// Its light baked (Material::Lightmaps), at the vertices' lightUv: two groups, each the light's
// square root in its color, its alpha where it's baked (none: 0 everywhere).
[[vk::binding(1, 0)]]
Sampler2D lightmap0;
[[vk::binding(2, 0)]]
Sampler2D lightmap1;
// This draw's lights at points (Renderer3D::AddPointLight), in its object's space: the first row's
// x how many, y and z its lighting's saturation and exposure, w 1 if it's graded; the second's x and
// y how bright each lightmap lights; then two rows each, where it is (w: how far it reaches) and its
// light, linear, by its brightness.
[[vk::binding(3, 0)]]
StructuredBuffer<float4> pointLights;
// A skinned mesh's joints this frame (Renderer3D::AddJoints): each joint's matrix as three rows
// (the fourth, 0 0 0 1, left out).
[[vk::binding(4, 0)]]
StructuredBuffer<float4> jointRows;
#endif

// Fog's density per unit of distance at an alpha of 255.
static const float FOG_DENSITY_SCALE = 1.0;
// Toon: where the light's side ends (the cosine toward the light) and how soft that edge is; the
// shadow's tint (a little cool and pink, as cartoons shade skin and cloth); the rim's band.
static const float TOON_EDGE = 0.12;
static const float TOON_SOFTNESS = 0.04;
static const float3 TOON_SHADE = float3(0.78, 0.7, 0.9);
static const float TOON_RIM_EDGE = 0.62;
// A light at a point also lights what faces away from it this much (light wraps round in a room);
// as a cartoon, its pool's edge as soft as this.
static const float POINT_WRAP = 0.35;
static const float TOON_POINT_SOFTNESS = 0.25;
// Graded, colours roll off from this toward white instead of clipping.
static const float SHOULDER = 0.8;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 color;
    [[vk::location(6)]] float2 lightUv;
};

// And the joints a skinned vertex moves with (up to four), and how much each moves it (summing to
// about 1; all 0 for a vertex that stays where it is).
struct SkinnedInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 color;
    [[vk::location(4)]] uint4 joints;
    [[vk::location(5)]] float4 weights;
    [[vk::location(6)]] float2 lightUv;
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
    float2 lightUv : TEXCOORD4;
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

// Brighter by `exposure`, richer or greyer by `saturation` (about its luminance), and past SHOULDER
// rolling off toward white.
float3 Grade(float3 color, float saturation, float exposure)
{
    color *= exposure;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = max(lerp(float3(luminance), color, saturation), float3(0.0));
    float3 over = max(color - float3(SHOULDER), float3(0.0));
    return min(color, float3(SHOULDER)) + over / (float3(1.0) + over / (1.0 - SHOULDER));
}

VertexOutput Place(float3 position, float3 normal, float2 uv, float4 color, float2 lightUv)
{
    VertexOutput output;
    output.position = constants.clip0 * position.x + constants.clip1 * position.y + constants.clip2 * position.z +
                      constants.clip3;
    output.objectPosition = position;
    output.normal = normal;
    output.uv = uv;
    output.color = color;
    output.viewDepth = output.position.w;
    output.lightUv = lightUv;
    return output;
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    return Place(input.position, input.normal, input.uv, input.color, input.lightUv);
}

#if !defined(HYOSHI_WGSL)
// Moved by its joints' matrices blended by weight (the weights made to sum to 1, as bytes rarely do
// exactly), as renderer/Skinning's SkinVertices does on the CPU.
[shader("vertex")]
VertexOutput vertexSkinned(SkinnedInput input)
{
    float total = dot(input.weights, float4(1.0));
    if (total <= 0.0)
    {
        return Place(input.position, input.normal, input.uv, input.color, input.lightUv);
    }
    float4 row0 = float4(0.0);
    float4 row1 = float4(0.0);
    float4 row2 = float4(0.0);
    for (int k = 0; k < 4; ++k)
    {
        float weight = input.weights[k] / total;
        if (weight > 0.0)
        {
            uint first = input.joints[k] * 3u;
            row0 += jointRows[first] * weight;
            row1 += jointRows[first + 1u] * weight;
            row2 += jointRows[first + 2u] * weight;
        }
    }
    float4 position = float4(input.position, 1.0);
    float3 normal = float3(dot(row0.xyz, input.normal), dot(row1.xyz, input.normal), dot(row2.xyz, input.normal));
    float length = sqrt(dot(normal, normal));
    return Place(float3(dot(row0, position), dot(row1, position), dot(row2, position)),
                 length > 0.0 ? normal / length : input.normal, input.uv, input.color, input.lightUv);
}
#endif

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
#if !defined(HYOSHI_WGSL)
    float4 grade = pointLights[0];
    // Baked: its light the lightmaps', each as bright as the lighting has it (and its rim, which the
    // light that baked it had none of).
    float4 baked0 = lightmap0.Sample(input.lightUv);
    uint count = uint(grade.x);
    if (baked0.a > 0.5)
    {
        float2 weights = pointLights[1].xy;
        float3 baked1 = lightmap1.Sample(input.lightUv).rgb;
        lit = albedo * (baked0.rgb * baked0.rgb * weights.x + baked1 * baked1 * weights.y) + light * rim;
        count = 0u;
    }
    // Each light at a point, falling off as light does (with the square of the distance, in
    // metres, from a metre's softness) and windowed to nothing at its reach; as a cartoon, its
    // pool's edge softened into a band.
    for (uint i = 0u; i < count; ++i)
    {
        float4 place = pointLights[2u + i * 2u];
        float3 glow = pointLights[3u + i * 2u].rgb;
        float3 toPoint = place.xyz - input.objectPosition;
        float distance = length(toPoint);
        float near = distance / max(place.w, 1.0e-4);
        float window = saturate(1.0 - near * near * near * near);
        float falloff = window * window / (1.0 + distance * distance);
        float wrap = saturate((dot(normal, toPoint / max(distance, 1.0e-4)) + POINT_WRAP) / (1.0 + POINT_WRAP));
        float toonWrap = smoothstep(0.5 - TOON_POINT_SOFTNESS, 0.5 + TOON_POINT_SOFTNESS, wrap) * 0.85 + 0.15;
        lit += albedo * glow * falloff * lerp(wrap, toonWrap, toon);
    }
#endif
    float3 color = lerp(lit, albedo, constants.lightDirection.w);

    float fogAmount = 1.0 - exp(-input.viewDepth * fog.a * FOG_DENSITY_SCALE);
    color = lerp(color, ToLinear(fog.rgb), fogAmount);
#if !defined(HYOSHI_WGSL)
    if (grade.w > 0.5)
    {
        color = Grade(color, grade.y, grade.z);
    }
#endif
    return float4(ToSrgb(color), base.a);
}
