#version 450

layout(location = 0) in vec2 v_uv;

layout(binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    vec4 src; // xy = active source size, zw = full image size
} pc;

layout(location = 0) out vec4 fragColor;

void main() {

    vec2 uv = v_uv * (pc.src.xy / pc.src.zw);
    fragColor = texture(tex, uv);
}