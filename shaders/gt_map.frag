#version 450

// A packed KSM1 mask: LSB0, row-major bit indices. The optional validity
// plane immediately follows the sky bit plane. This avoids CPU expansion.
layout(set = 0, binding = 0) uniform sampler2D skyPacked;
layout(set = 0, binding = 1) uniform sampler2D metricDepth;

layout(push_constant) uniform Params {
    vec4 opacityAndRange; // sky opacity, depth opacity, min Z, max Z (metres)
    ivec4 dims;           // sky width/height, depth width/height
    ivec4 flags;          // sky ready, depth ready, has validity, reserved
} p;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

uint maskBit(int bitIndex) {
    const int byteIndex = bitIndex >> 3;
    ivec2 texel = ivec2(byteIndex % 1024, byteIndex / 1024);
    uint packed = uint(round(texelFetch(skyPacked, texel, 0).r * 255.0));
    return (packed >> uint(bitIndex & 7)) & 1u;
}

vec3 depthColor(float t) {
    // Perceptually ordered blue/cyan/yellow/red visualization of optical Z.
    // 0 = near, 1 = far; never normalize using per-frame min/max.
    const vec3 nearColor = vec3(0.98, 0.30, 0.16);
    const vec3 midColor  = vec3(0.96, 0.84, 0.18);
    const vec3 farColor  = vec3(0.12, 0.49, 0.96);
    if (t < 0.5) return mix(nearColor, midColor, t * 2.0);
    return mix(midColor, farColor, (t - 0.5) * 2.0);
}

void main() {
    float opacity = 0.0;
    vec3 color = vec3(0.0);

    if (p.flags.y != 0 && p.dims.z > 0 && p.dims.w > 0) {
        ivec2 xy = clamp(ivec2(inUv * vec2(p.dims.zw)),
                         ivec2(0), p.dims.zw - ivec2(1));
        // KDM1 is optical-axis Z in metres (not Euclidean root range_m).
        float z = texelFetch(metricDepth, xy, 0).r;
        if (!isnan(z) && !isinf(z) && z > 0.0) {
            float t = clamp((z - p.opacityAndRange.z) /
                             max(p.opacityAndRange.w - p.opacityAndRange.z, 0.01),
                            0.0, 1.0);
            color = depthColor(t);
            opacity = p.opacityAndRange.y;
        }
    }

    if (p.flags.x != 0 && p.dims.x > 0 && p.dims.y > 0) {
        ivec2 xy = clamp(ivec2(inUv * vec2(p.dims.xy)),
                         ivec2(0), p.dims.xy - ivec2(1));
        int bitIndex = xy.y * p.dims.x + xy.x;
        int planeBytes = (p.dims.x * p.dims.y + 7) / 8;
        bool valid = p.flags.z == 0 ||
                     maskBit(planeBytes * 8 + bitIndex) != 0u;
        if (valid && maskBit(bitIndex) != 0u) {
            float skyAlpha = p.opacityAndRange.x;
            float combined = skyAlpha + opacity * (1.0 - skyAlpha);
            if (combined > 0.0)
                color = (vec3(0.15, 0.93, 0.97) * skyAlpha +
                         color * opacity * (1.0 - skyAlpha)) / combined;
            opacity = combined;
        }
    }
    outColor = vec4(color, opacity);
}
