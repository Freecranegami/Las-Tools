#version 450
#extension GL_GOOGLE_include_directive : enable
// FSR 1, pass 2: RCAS (robust contrast adaptive sharpening) from AMD FidelityFX FSR 1.0
// (github.com/GPUOpen-Effects/FidelityFX-FSR, MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.).
// The constant is made on the CPU in sgsr.c (FsrRcasCon): exp2(-stops), where 0 stops is the sharpest.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { uvec4 con; } pc;
layout(location = 0) out vec4 outColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(tex, ASU2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(gl_FragCoord.xy), pc.con);
    outColor = vec4(c, 1.0);
}
