# PVRTC texture decoder

`PVRTDecompress.cpp` and `PVRTDecompress.h` are copied from Imagination Technologies'
PowerVR Native SDK at the exact revision recorded in `revision.txt`. The SDK's MIT license is retained in `LICENSE.md`. The only local source edit is a
`[[maybe_unused]]` annotation on the decoder's power-of-two assertion helper, because
release builds compile those assertions out and the Android build treats warnings as errors.

AnyiOS uses `pvr::PVRTDecompressPVRTC` only. The bundled ANGLE Vulkan build does not
implement PVRTC Vulkan formats, so the GLES bridge decodes PVRTC1 RGB/RGBA 2-bpp and
4-bpp uploads into bounded RGBA8 host memory before submitting them to GLES. The SDK's
ETC decoder is present in the same upstream source file but is not called here.
