#version 450

layout(set = 0, binding = 0) uniform sampler2D videoTexture;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 rgb = texture(videoTexture, inUv).rgb;
    outColor = vec4(rgb, 1.0);
}
