#version 450
// Deterministic animated scene: 8x18-cell checkerboard over the surface plus a moving bar, so a
// screenshot at a known time is reproducible for image-quality comparison, and fine vertical
// structures make resolution loss visible.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform PC { float u_time; } pc;
void main() {
    vec2 cell = floor(v_uv * vec2(24.0, 54.0));
    float checker = mod(cell.x + cell.y, 2.0);
    vec3 base = mix(vec3(0.16, 0.20, 0.28), vec3(0.86, 0.88, 0.92), checker);
    float bar = step(0.48, fract(v_uv.x - pc.u_time * 0.13)) * step(fract(v_uv.x - pc.u_time * 0.13), 0.52);
    base = mix(base, vec3(1.0, 0.35, 0.15), bar * 0.85);
    float ring = smoothstep(0.28, 0.26, length(v_uv - vec2(0.5 + 0.25 * sin(pc.u_time * 0.7), 0.5)));
    base = mix(base, vec3(0.10, 0.65, 0.95), ring);
    outColor = vec4(base, 1.0);
}
