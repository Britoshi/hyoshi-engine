#pragma once

// shaders/DebugUi.slang as GLSL ES 3.00, for WebGL 2 (engine/rhi/webgl), where WebGPU isn't
// available: keep the two in step. The constants are the std140 block Constants, and the
// texture is texture0. Clip space y points up, as in WebGPU.

namespace hyoshi::debug
{

inline constexpr const char* DEBUG_UI_GLES_VERTEX = R"(#version 300 es
precision highp float;

layout(std140) uniform Constants
{
    vec2 scale;
    vec2 offset;
    vec2 rotation;
};

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;

out vec2 vUv;
out vec4 vColor;

void main()
{
    vec2 p = position * scale + offset;
    gl_Position = vec4(p.x * rotation.x - p.y * rotation.y, -(p.x * rotation.y + p.y * rotation.x), 0.0, 1.0);
    vUv = uv;
    vColor = color;
}
)";

inline constexpr const char* DEBUG_UI_GLES_FRAGMENT = R"(#version 300 es
precision highp float;

uniform sampler2D texture0;

in vec2 vUv;
in vec4 vColor;

out vec4 fragColor;

void main()
{
    fragColor = vColor * texture(texture0, vUv);
}
)";

} // namespace hyoshi::debug
