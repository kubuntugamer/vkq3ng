FSR2 backend
============

This directory contains the AMD FidelityFX Super Resolution 2.2.1 runtime
(`ffx_fsr2.cpp`, `ffx_fsr2_vk.cpp`, headers, and shader helper headers),
taken from <https://github.com/GPUOpen-Effects/FidelityFX-FSR2> at tag
`v2.2.1` and covered by `LICENSE.txt` (MIT).

The Vulkan shader blobs in `vk/shaders/ffx_fsr2_shaders_vk_data.h` were built
offline from the upstream GLSL sources with `glslangValidator`; the FidelityFX
shader compiler is not needed at build time.  The build uses one fixed
permutation: HDR color input, low-resolution motion vectors, non-jittered
motion vectors, non-inverted depth, Lanczos-2, `FFX_HALF=0`, and the two
sharpening variants.  The blob generator and runtime are selected by
`fsr2GetPermutationBlobByIndexVK()`.

The Quake renderer talks to this backend through
`code/renderer/vulkan/vk_fsr2.cpp`; it copies the RT G-buffers to render-size
images before dispatch because FSR2 samples normalized UVs and cannot read a
sub-rectangle of the larger G-buffer allocations.
