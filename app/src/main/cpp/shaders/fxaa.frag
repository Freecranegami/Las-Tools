#version 450
// FXAA (Fast approximate Anti-Aliasing). The algorithm is Timothy Lottes' FXAA (NVIDIA): find the edge direction from the
// luma of the four diagonal neighbours, then blend two short samples along it; the extra early-out leaves flat areas
// and texture detail untouched. Written for this project from the published algorithm.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

const float REDUCE_MIN = 1.0 / 128.0;
const float REDUCE_MUL = 1.0 / 8.0;
const float SPAN_MAX = 8.0;
const float EDGE_MIN = 1.0 / 24.0;       // below this local contrast nothing is done
const float EDGE_REL = 1.0 / 8.0;        // ... or below this fraction of the brightest neighbour

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 inv = 1.0 / vec2(textureSize(src, 0));
    vec4 m = texture(src, uv);
    vec3 rgbM = m.rgb;
    vec3 rgbNW = texture(src, uv + vec2(-1.0, -1.0) * inv).rgb;
    vec3 rgbNE = texture(src, uv + vec2( 1.0, -1.0) * inv).rgb;
    vec3 rgbSW = texture(src, uv + vec2(-1.0,  1.0) * inv).rgb;
    vec3 rgbSE = texture(src, uv + vec2( 1.0,  1.0) * inv).rgb;
    float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE), lM = luma(rgbM);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(EDGE_MIN, lMax * EDGE_REL)) { outColor = m; return; }

    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * REDUCE_MUL), REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-SPAN_MAX), vec2(SPAN_MAX)) * inv;

    vec3 rgbA = 0.5 * (texture(src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(src, uv + dir * -0.5).rgb + texture(src, uv + dir * 0.5).rgb);
    float lB = luma(rgbB);
    outColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, m.a);
}
