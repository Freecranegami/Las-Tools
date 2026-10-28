#version 450
#extension GL_GOOGLE_include_directive : enable
// FSR 1, pass 1: EASU (edge-adaptive spatial upscaling) from AMD FidelityFX FSR 1.0
// (github.com/GPUOpen-Effects/FidelityFX-FSR, MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.).
// The functions come unchanged from shaders/ffx/ffx_a.h and ffx_fsr1.h; this file is the fragment-shader wrapper
// (the AMD sample uses a compute shader). The constants are made on the CPU in sgsr.c (FsrEasuCon).
layout(set = 0, binding = 0) uniform sampler2D tex;                   // bilinear, clamp to edge
layout(push_constant) uniform PC { uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; } pc;
layout(location = 0) out vec4 outColor;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(tex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(tex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(tex, p, 2); }
#include "ffx_fsr1.h"

void main() {
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), pc.con0, pc.con1, pc.con2, pc.con3);
    outColor = vec4(c, 1.0);
}
