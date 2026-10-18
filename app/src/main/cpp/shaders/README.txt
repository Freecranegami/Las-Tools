cas_spv.h (one directory up) holds the compiled SPIR-V of these two shaders:
  glslangValidator -V cas.vert -o cas.vert.spv ; glslangValidator -V cas.frag -o cas.frag.spv
then each .spv file is written out as a uint32_t array (cas_vert_spv, cas_frag_spv).
cas.frag follows AMD FidelityFX CAS (MIT licence, Copyright (c) 2019 Advanced Micro Devices, Inc.).
