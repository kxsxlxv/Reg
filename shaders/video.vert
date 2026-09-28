#version 450

layout(location = 0) out vec2 outUv;

void main() {
    vec2 uv = vec2(
        float((gl_VertexIndex << 1) & 2),
        float(gl_VertexIndex & 2));

    outUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
