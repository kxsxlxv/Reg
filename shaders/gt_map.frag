#version 450

// A packed KSM1 mask: LSB0, row-major bit indices. The optional validity
// plane immediately follows the sky bit plane. This avoids CPU expansion.
layout(set = 0, binding = 0) uniform sampler2D skyPacked;
layout(set = 0, binding = 1) uniform sampler2D metricDepth;

layout(push_constant) uniform Params {
    vec4 opacityAndRange; // sky opacity, depth opacity, min Z, max Z (metres)
    ivec4 dims;           // sky width/height, depth width/height
    ivec4 flags;          // sky ready, depth ready, has validity, filter bits
} p;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

uint maskBit(int bitIndex) {
    const int byteIndex = bitIndex >> 3;
    ivec2 texel = ivec2(byteIndex % 1024, byteIndex / 1024);
    uint packed = uint(round(texelFetch(skyPacked, texel, 0).r * 255.0));
    return (packed >> uint(bitIndex & 7)) & 1u;
}

// KSM1 is packed LSB0. Never linearly sample packed bytes: interpolate the
// decoded binary coverage instead. Invalid/unknown pixels contribute alpha 0.
float skyAt(ivec2 pixel) {
    pixel = clamp(pixel, ivec2(0), p.dims.xy - ivec2(1));
    int bitIndex = pixel.y * p.dims.x + pixel.x;
    int planeBytes = (p.dims.x * p.dims.y + 7) / 8;
    if (p.flags.z != 0 && maskBit(planeBytes * 8 + bitIndex) == 0u)
        return 0.0;
    return float(maskBit(bitIndex));
}

float sampleSky(vec2 uv) {
    if ((p.flags.w & 1) == 0) {
        return skyAt(ivec2(uv * vec2(p.dims.xy)));
    }
    vec2 texel = uv * vec2(p.dims.xy) - vec2(0.5);
    ivec2 base = ivec2(floor(texel));
    vec2 f = fract(texel);
    float a = mix(skyAt(base), skyAt(base + ivec2(1, 0)), f.x);
    float b = mix(skyAt(base + ivec2(0, 1)),
                  skyAt(base + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
}

float depthAt(ivec2 pixel) {
    pixel = clamp(pixel, ivec2(0), p.dims.zw - ivec2(1));
    float z = texelFetch(metricDepth, pixel, 0).r;
    return (isnan(z) || isinf(z) || z <= 0.0) ? 0.0 : z;
}

float sampleDepth(vec2 uv) {
    if ((p.flags.w & 2) == 0)
        return depthAt(ivec2(uv * vec2(p.dims.zw)));
    vec2 texel = uv * vec2(p.dims.zw) - vec2(0.5);
    ivec2 base = ivec2(floor(texel));
    vec2 f = fract(texel);
    float z00 = depthAt(base);
    float z10 = depthAt(base + ivec2(1, 0));
    float z01 = depthAt(base + ivec2(0, 1));
    float z11 = depthAt(base + ivec2(1, 1));
    vec4 w = vec4((1.0-f.x)*(1.0-f.y), f.x*(1.0-f.y),
                  (1.0-f.x)*f.y, f.x*f.y);
    // Do not blend invalid optical-Z (zero) into nearby valid metres.
    vec4 valid = vec4(z00>0.0, z10>0.0, z01>0.0, z11>0.0);
    vec4 vw = w * valid;
    float sum = dot(vw, vec4(1.0));
    if (sum <= 0.0) return 0.0;
    return dot(vw, vec4(z00, z10, z01, z11)) / sum;
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
        // Normalised RGB UV stretches the whole source map to the Raw
        // video viewport, e.g. KDM1 480x270 onto RGB 1920x1080.
        // KDM1 is optical-axis Z (not Euclidean object range_m).
        float z = sampleDepth(inUv);
        if (!isnan(z) && !isinf(z) && z > 0.0) {
            float t = clamp((z - p.opacityAndRange.z) /
                             max(p.opacityAndRange.w - p.opacityAndRange.z, 0.01),
                            0.0, 1.0);
            color = depthColor(t);
            opacity = p.opacityAndRange.y;
        }
    }

    if (p.flags.x != 0 && p.dims.x > 0 && p.dims.y > 0) {
        // Sky bitplane covers the entire RGB image; filter the decoded
        // coverage (not the packed texture bytes) at the chosen quality.
        float skyAlpha = p.opacityAndRange.x * sampleSky(inUv);
        if (skyAlpha > 0.0) {
            float combined = skyAlpha + opacity * (1.0 - skyAlpha);
            if (combined > 0.0)
                color = (vec3(0.15, 0.93, 0.97) * skyAlpha +
                         color * opacity * (1.0 - skyAlpha)) / combined;
            opacity = combined;
        }
    }
    outColor = vec4(color, opacity);
}
