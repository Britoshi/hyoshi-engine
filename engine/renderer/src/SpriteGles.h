#pragma once

// shaders/Sprite.slang as GLSL ES 3.00, for WebGL 2 (engine/rhi/webgl), where WebGPU isn't
// available: keep the two in step. The constants are the std140 block Constants, and the
// texture is texture0. Clip space y points up, as in WebGPU.

namespace hyoshi::renderer
{

inline constexpr const char* SPRITE_GLES_VERTEX = R"(#version 300 es
precision highp float;
precision highp int;

layout(std140) uniform Constants
{
    vec2 scale;
    vec2 offset;
    vec2 rotation;
};

layout(location = 0) in vec4 topCorners;
layout(location = 1) in vec4 bottomCorners;
layout(location = 2) in vec4 uvRect;
layout(location = 3) in vec4 color;
layout(location = 4) in uint flags;
layout(location = 5) in vec4 clipRect;

out vec2 vUv;
out vec4 vColor;
flat out uint vFlags;
out vec2 vVirtualPosition;
flat out vec4 vClipRect;
out vec3 vEdges;

vec2 ToClip(vec2 position)
{
    vec2 p = position * scale + offset;
    return vec2(p.x * rotation.x - p.y * rotation.y, p.x * rotation.y + p.y * rotation.x);
}

void main()
{
    uint vertexId = uint(gl_VertexID);
    vec2 corner = vec2(float(vertexId & 1u), float(vertexId >> 1u));
    vec4 edge = (vertexId >> 1u) != 0u ? bottomCorners : topCorners;
    vec2 position = (vertexId & 1u) != 0u ? edge.zw : edge.xy;
    vec2 clip = ToClip(position);
    gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);
    vUv = (flags & 2u) != 0u ? (position - uvRect.xy) / (uvRect.zw - uvRect.xy) : mix(uvRect.xy, uvRect.zw, corner);
    vEdges = vec3(0.0);
    if ((flags & 4u) != 0u)
    {
        vec2 a = topCorners.xy;
        vec2 b = topCorners.zw;
        vec2 c = bottomCorners.xy;
        vec2 ab = b - a;
        vec2 ac = c - a;
        float area = abs((ab.x * ac.y) - (ab.y * ac.x));
        vec3 heights = area / max(vec3(length(c - b), length(a - c), length(b - a)), vec3(1.0e-4));
        vEdges = vertexId == 0u ? vec3(heights.x, 0.0, 0.0)
                 : vertexId == 1u ? vec3(0.0, heights.y, 0.0)
                                  : vec3(0.0, 0.0, heights.z);
        vUv = uvRect.xy;
    }
    vColor = color;
    vFlags = flags;
    vVirtualPosition = position;
    vClipRect = clipRect;
}
)";

inline constexpr const char* SPRITE_GLES_FRAGMENT = R"(#version 300 es
precision highp float;
precision highp int;

uniform sampler2D texture0;

in vec2 vUv;
in vec4 vColor;
flat in uint vFlags;
in vec2 vVirtualPosition;
flat in vec4 vClipRect;
in vec3 vEdges;

out vec4 fragColor;

const uint DISTANCE_FIELD = 1u;
const uint EDGE_GLOW = 4u;

void main()
{
    vec4 texel = texture(texture0, vUv);
    float field = texel.a;
    float width = max(fwidth(field) * 0.7, 1.0e-4);
    vec2 p = vVirtualPosition;
    float inside = (p.x >= vClipRect.x && p.y >= vClipRect.y && p.x <= vClipRect.z && p.y <= vClipRect.w) ? 1.0 : 0.0;
    float edge = min(vEdges.x, min(vEdges.y, vEdges.z));
    float edgeWidth = max(fwidth(edge), 1.0e-4);
    if ((vFlags & EDGE_GLOW) != 0u)
    {
        float glow = 1.0 - clamp(edge / max(vUv.x, 1.0e-4), 0.0, 1.0);
        float light = mix(vUv.y, 1.0, glow * glow) * clamp(edge / edgeWidth, 0.0, 1.0);
        fragColor = vec4(vColor.rgb, vColor.a * light * inside);
        return;
    }
    if ((vFlags & DISTANCE_FIELD) != 0u)
    {
        float coverage = smoothstep(0.5 - width, 0.5 + width, field);
        fragColor = vec4(vColor.rgb, vColor.a * coverage * inside);
        return;
    }
    vec4 shaded = texel * vColor;
    fragColor = vec4(shaded.rgb, shaded.a * inside);
}
)";

} // namespace hyoshi::renderer
