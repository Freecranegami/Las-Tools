#version 450
// Contrast Adaptive Sharpening, a GLSL port of CasFilter from AMD FidelityFX CAS (MIT licence,
// Copyright (c) 2019 Advanced Micro Devices, Inc.).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { float sharpness; } pc;
layout(location = 0) out vec4 outColor;

vec3 tap(ivec2 p, ivec2 hi) {
    return texelFetch(src, clamp(p, ivec2(0), hi), 0).rgb;
}

void main() {
    ivec2 hi = textureSize(src, 0) - 1;
    ivec2 p = ivec2(gl_FragCoord.xy);
    //  a b c
    //  d e f
    //  g h i
    vec3 a = tap(p + ivec2(-1, -1), hi);
    vec3 b = tap(p + ivec2( 0, -1), hi);
    vec3 c = tap(p + ivec2( 1, -1), hi);
    vec3 d = tap(p + ivec2(-1,  0), hi);
    vec3 e = tap(p, hi);
    vec3 f = tap(p + ivec2( 1,  0), hi);
    vec3 g = tap(p + ivec2(-1,  1), hi);
    vec3 h = tap(p + ivec2( 0,  1), hi);
    vec3 i = tap(p + ivec2( 1,  1), hi);

    // Soft min and max: the plus shape and the full 3x3, added together.
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(min(min(mn, a), min(c, g)), i);
    mn = mn + mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(max(max(mx, a), max(c, g)), i);
    mx = mx + mx2;

    // Smooth minimum distance to the signal limit, divided by the smooth max.
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0);
    amp = sqrt(amp);

    // Filter shape:  0 w 0 / w 1 w / 0 w 0
    float peak = -1.0 / mix(8.0, 5.0, clamp(pc.sharpness, 0.0, 1.0));
    vec3 w = amp * peak;
    vec3 rcpW = 1.0 / (1.0 + 4.0 * w);
    vec3 col = clamp((b * w + d * w + f * w + h * w + e) * rcpW, 0.0, 1.0);
    outColor = vec4(col, texelFetch(src, p, 0).a);
}
