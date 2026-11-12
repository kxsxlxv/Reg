#version 450

layout(set = 0, binding = 0) uniform sampler2D videoTexture;

// FFmpeg hardware decode can allocate an aligned VkImage (e.g. 1920x1088)
// for a 1920x1080 visible picture. The full [0,1] texture UV range includes
// padding rows; only the visible region is mapped into the output viewport.
layout(push_constant) uniform VideoSampleRegion {
    vec4 uvRect; // u_min, v_min, u_max, v_max
} sampleRegion;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 visibleUv = mix(sampleRegion.uvRect.xy, sampleRegion.uvRect.zw, inUv);
    vec3 rgb = texture(videoTexture, visibleUv).rgb;
    outColor = vec4(rgb, 1.0);
}
