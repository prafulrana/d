/* d native bake — single-process, single-GPU, one-pass-per-frame.
 *
 *   libavformat (HTTP via VPN proxy)
 *     → libavcodec *_cuvid       (NVDEC → AVFrame format=AV_PIX_FMT_CUDA)
 *       → nv12_pre_vsr_filter kernel  (NV12/P010/P016 CUDA + deblock → source-size RGBA8)
 *         → source-size RGBA prep
 *           → NVIDIA Optical Flow + CUDA FRUC on source-size RGBA, when the
 *             manifest puts nvof_fruc before upscaler
 *             → NvVFX / TRT upscaler to 4K RGBA
 *               → manifest-ordered 4K post + temporal stages
 *                 → rgba_to_nv12 kernel  (final RGBA → NV12 CUDA 4K)
 *                   → hevc_nvenc         (AVFrame format=CUDA, sw=NV12)
 *                     → libavformat MPEG-TS (canon HEVC)
 *
 * No libavfilter, no torch, no scale_cuda RGB bridge. Maxine SR runs before
 * subjective filters; all denoise/eq/sharpen/grain work is on the 4K canvas.
 * Audio is either AAC transport without Maxine (Pass) or real Maxine AFX:
 * light cleanup at 16 kHz plus Audio Super Resolution 16→48 kHz before
 * the transport audio encoder. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>

#include <cuda.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "include/nvVideoEffects.h"
#include "include/nvCVImage.h"
#include "include/nvCVStatus.h"
#include "include/nvVFXVideoSuperRes.h"
#include "nvAudioEffects.h"
#include "dereverb_denoiser.h"
#include "superres.h"
#include "jsmn.h"
#include "bake.h"
#include "bake_internal.h"
#include "pipeline_manifest.h"
#include "trt_sr_engine.h"
#include "perf_ring.h"

#include <sys/mman.h>      // mlockall, madvise
#include <sys/resource.h>  // setrlimit
#include <fcntl.h>         // posix_fadvise
#include <dirent.h>        // DIR, readdir for NGX cache prefault
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

extern void launch_nv12_pre_vsr_filter(
    const void* y_plane, const void* uv_plane, void* out_rgba,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int input_format,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float cas_strength,
    void* stream);

extern void launch_post_vsr_finalize_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float grain_strength,
    uint32_t frame_seed,
    float sharpen_strength,
    float contrast_boost,
    void* stream);

extern void launch_rgba_to_nv12(
    const void* in_rgba, void* out_y, void* out_uv,
    int H, int W,
    int y_pitch, int uv_pitch,
    void* stream);

extern void launch_nvof_fruc_interpolate(
    const void* prev_rgba, const void* curr_rgba,
    const void* forward_flow_vectors,
    const void* reverse_flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float alpha,
    float confidence_scale,
    void* stream);

extern void launch_temporal_reconstruct_rgba(
    const void* prev_rgba, const void* curr_rgba,
    const void* forward_flow_vectors,
    const void* reverse_flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float temporal_strength,
    float edge_stability,
    void* stream);

extern void launch_rgba_to_abgr_pitch(
    const void* in_rgba,
    void* out_abgr,
    int H, int W,
    int out_pitch_bytes,
    void* stream);

extern void launch_rgba8_to_rgb_chw_resampled(
    const void* in_rgba,
    void* out_rgb_chw_fp32,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream);

extern void launch_rgb_chw_to_rgba8_resampled(
    const void* in_rgb_chw_fp32,
    void* out_rgba,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream);

extern void launch_rgb_chw_to_rgb_chw_resampled(
    const void* in_rgb_chw_fp32,
    void* out_rgb_chw_fp32,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream);

extern void launch_rgba8_to_rgba8_resampled(
    const void* in_rgba,
    void* out_rgba,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream);

extern void launch_deband_4k_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W, float strength, uint32_t frame_seed, void* stream);

extern void launch_temporal_denoise_rgba(
    const void* curr_rgba, const void* prev_rgba,
    const void* flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int flow_pitch_bytes,
    int flow_grid,
    float strength,
    void* stream);

// Forward declarations for helpers defined further down.
struct AVFrame;
static void bake_write_runtime_state(BakeWorker* w, int src_w, int src_h, int active_bin);
static void destroy_trt_upscaler_engines(BakeWorker* w);

#define BAIL(msg) do { snprintf(g_last_error, sizeof(g_last_error), "%s", msg); goto bail; } while(0)
#define BAIL_FMT(fmt, ...) do { snprintf(g_last_error, sizeof(g_last_error), fmt, __VA_ARGS__); goto bail; } while(0)
#define CHECK_CU(call, what) do { CUresult _r = (call); if (_r != CUDA_SUCCESS) BAIL_FMT("cuda_%s_err=%d", what, _r); } while(0)
#define CHECK_NV(call, what) do { NvCV_Status _s = (call); if (_s != NVCV_SUCCESS) BAIL_FMT("vfx_%s_err=%d", what, _s); } while(0)
#define CHECK_OF(call, what) do { NV_OF_STATUS _s = (call); if (_s != NV_OF_SUCCESS) BAIL_FMT("nvof_%s_err=%d", what, _s); } while(0)
#define CHECK_AV(call, what) do { int _r = (call); if (_r < 0) { char _b[128]; av_strerror(_r, _b, sizeof(_b)); BAIL_FMT("av_%s_err=%s", what, _b); } } while(0)

enum {
    DPROC_YUV420_NV12 = 0,
    DPROC_YUV420_P010 = 1,
    DPROC_YUV420_P016 = 2,
};

__thread char g_last_error[512];
static const char* KODI_ANDROID_UA =
    "Kodi/21.2 (Linux; Android 12; Pixel 7) Version/21.2-(21.2-Omega)";
static const char* KODI_ANDROID_HEADERS =
    "Referer: Kodi (Android)\r\n"
    "Accept: */*\r\n"
    "Accept-Language: en-US,en;q=0.8\r\n"
    "Connection: keep-alive\r\n";

const char* bake_get_last_error(void) { return g_last_error; }

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ----------------------------------------------------------------------------
// VSR NvCVImage views over our pre-allocated RGBA8 buffers. Hoisted so the
// worker init can bind input/output images to the effect before NvVFX_Load —
// the SDK needs the resolved input/output dimensions at Load time to pick the
// right model graph; otherwise Load returns NVCV_ERR_INITIALIZATION (-12).
// ----------------------------------------------------------------------------
int wrap_rgba_nvcv(NvCVImage* im, void* ptr, int W, int H) {
    NvCV_Status s = NvCVImage_Init(im, (unsigned)W, (unsigned)H,
                                    (int)(W * 4),         // pitch in bytes
                                    ptr,
                                    NVCV_RGBA, NVCV_U8,
                                    NVCV_INTERLEAVED,
                                    NVCV_GPU);
    if (s != NVCV_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "nvcv_init_rgba_err=%d", s);
        return -1;
    }
    im->pixels = ptr;
    return 0;
}
