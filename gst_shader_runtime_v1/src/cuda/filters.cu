// d fused filter kernels — native path through the canon filter chain.
//
// One native GPU chain. Each stage reads its input once, writes once.
// Intermediate values stay in registers; no CPU frame path or ffmpeg filters.
//
//   nv12_pre_vsr_filter:   CUDA NV12/P010/P016 decoder frame
//                       →   compression-edge deblock at 8/16-pixel boundaries
//                       →   BT.709 color convert at source-size
//                       →   RGBA8 interleaved (source-size, ready for VSR)
//
//   post_vsr_finalize:     RGBA8 interleaved (4K, post-VSR)
//                       →  spatial denoise on the 4K SR output
//                       →  eq (gamma → contrast → saturation, BT.709)
//                       →  FXAA-style line anti-alias + dehalo/ringing clamp
//                       →  local-contrast enhancement (edge-gated mid-band
//                          detail boost — the "Sony Bravia" pop)
//                       →  edge-aware adaptive sharpen with compression guard
//                          (CAS unsharp + min/max clamp to 3×3 neighborhood —
//                          no overshoot ringing)
//                       →  filmic tone curve, shadow saturation rolloff,
//                          warm/cool split tone, subtle vignette
//                       →  clean film grain (per-pixel hash-based PRNG, no state)
//                       →  final RGBA8 interleaved
//
//   nvof_fruc_interpolate: prev final RGBA + current final RGBA + NVOF vectors
//                       →  motion-compensated 60 fps in-between final RGBA
//
//   temporal_reconstruct: prev final RGBA + current final RGBA + NVOF vectors
//                       →  DLSAA-style conservative temporal reconstruction
//                          for stable subpixel edges / reduced shimmer
//
//   rgba_to_nv12:          final/interpolated RGBA → NV12 CUDA frame
//
// All subjective filters used to live before VSR. That was the wrong order:
// filtering 1080p input asks Maxine to upscale our artifacts. The correct live
// path is SR first, then denoise/eq/CAS/grain on the 4K canvas.
//
// Memory layout:
//   VSR input/output:         RGBA uint8 interleaved, pixel stride = 4 bytes
//   Decoder input:            NV12 or high-bit-depth 4:2:0 CUDA surfaces
//   Encoder output:           NV12, Y plane + interleaved UV plane
//
// Filter constants match the existing d canon chain so we don't change the look:
//   hqdn3d ≈ 3×3 gaussian:   center=4, edge=2, corner=1, divisor=16
//   eq:                      contrast=1.05, saturation=1.08, gamma=0.97
//   sharpen strength:        0.28  (clamped by 3×3 min/max neighborhood)
//   local contrast boost:    0.25  (edge-gated by Sobel magnitude on luma)
//   film grain stddev:       3/255 ≈ 0.0118

#include <cstdint>
#include <cuda_runtime.h>

extern "C" {
#include "filters/00_common.inc.cu"
#include "filters/10_pre_vsr.inc.cu"
#include "filters/20_post_vsr.inc.cu"
#include "filters/30_temporal_and_launchers.inc.cu"
#include "filters/40_trt_bridge.inc.cu"
#include "filters/50_deband.inc.cu"
#include "filters/60_custom_shader_fxaa.inc.cu"
#include "filters/70_temporal_denoise.inc.cu"
}  // extern "C"
