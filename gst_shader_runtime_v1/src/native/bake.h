#ifndef DPROC_BAKE_H
#define DPROC_BAKE_H

#include <stdint.h>
#include <cuda.h>
#include "include/nvCVImage.h"
#include "include/nvVideoEffects.h"
#include "include/nvOpticalFlowCuda.h"
#include "pipeline_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// d native bake — single-process, single-GPU, one-pass-per-frame.
// Decoder → plain pre-VSR colorspace kernel → selected JSON-graph upscaler
// → fused 4K post filter chain → graph-placed NVOF/DLSAA → encoder.
// Audio is source-aware: narrowband sources use the real Maxine Audio Effects
// Audio Super Resolution chain, while normal full-band stereo program audio
// stays on the 48 kHz stereo bed with light EQ/limiter before mux. No torch,
// no nvvfx Python wrapper. The binary reads ONE JSON request line on stdin
//   { url, start_seconds, http_proxy?, user_agent?, headers?, output_url? }
// and streams canon HEVC/AAC to an encoded sink. RTSP output publishes
// directly to the media edge; stdout MPEG-TS is kept for diagnostic use only.
// Telemetry goes to stderr only.
// ----------------------------------------------------------------------------

// Canon-pinned output dimensions. VSR input dimensions come from the selected
// source stream; do not pre-stretch a low rendition to 1080p before Maxine.
#define BAKE_OUTPUT_WIDTH  3840
#define BAKE_OUTPUT_HEIGHT 2160

#define DPROC_AUDIO_SR_AUTO  0  // 8/16 kHz or narrowband mono only
#define DPROC_AUDIO_SR_FORCE 1  // force Maxine AudioSR even for 48 kHz stereo
#define DPROC_AUDIO_SR_OFF   2  // never run Maxine AudioSR

#define DPROC_AUDIO_PACING_SOURCE_PTS  0  // audio encodes from source/output clock only
#define DPROC_AUDIO_PACING_VIDEO_GATED 1  // live audio may lead video only by max_audio_lead_ms

typedef struct {
    int worker_ready;
    CUcontext cu_ctx;
    CUstream cu_stream;       // TRT upscaler compute stream (A)
    CUstream cu_stream_b;     // Downstream stream — resample/post/temporal/FRUC/rgba_to_nv12 (B)
    CUevent  ev_trt_done;  // Recorded on stream A after TRT upscaler N; stream B waits on it
    CUevent  ev_vsr_done;     // Cross-stream dependency for Maxine/source-FRUC handoffs
    CUevent  ev_nvof_done;    // Recorded on NVOF/output stream; downstream waits without CPU sync
    CUevent  ev_encode_ready; // Recorded after NV12 pack; FFmpeg/NVENC stream waits without CPU sync

    // Pipeline (all GPU, zero CPU round-trips):
    //   pre_vsr feeder (CUDA) → RGBA source-size (pre_vsr_rgba)
    //   vsr_effect            → RGBA 3840×2160 (vsr_out_rgba) — Maxine VSR ULTRA.
    //   post finalize (CUDA)  → final-look RGBA 3840×2160 (final_rgba).
    //   temporal reconstruction → DLSAA-style history clamp / edge stability.
    //   NVOF FRUC + CUDA      → motion-compensated final RGBA in-betweens.
    //   NV12 pack (CUDA)      → NV12 for NVENC.
    NvVFX_Handle vsr_effect;
    NvVFX_StateObjectHandle vsr_state;

    // GPU buffers (lifetime = worker, recreated per bake only on dim change)
    CUdeviceptr pre_vsr_rgba;         // RGBA8 interleaved source-size×4
    size_t      pre_vsr_rgba_bytes;
    int         vsr_input_width;
    int         vsr_input_height;
    int         maxine_input_width;
    int         maxine_input_height;
    CUdeviceptr vsr_out_rgba;         // RGBA8 interleaved 3840×2160×4, raw upscaler output normalized to 4K
    size_t      vsr_out_bytes;
    CUdeviceptr maxine_raw_rgba;      // scratch; Maxine sub-4K output is forbidden
    int         vsr_output_width;
    int         vsr_output_height;
    CUdeviceptr final_rgba;           // post-filter final-look 4K RGBA frame
    CUdeviceptr prev_final_rgba;      // previous final-look 4K RGBA frame for temporal post filters
    CUdeviceptr interp_final_rgba;    // 4K scratch for temporal/deband output

    // NVIDIA Optical Flow engine state. Input buffers are driver-owned CUDA
    // device pointers; final RGBA frames are copied into them and NVOF writes
    // S10.5 flow vectors used by the interpolation kernel.
    void*       nvof_lib;
    NV_OF_CUDA_API_FUNCTION_LIST nvof_api;
    NvOFHandle nvof_handle;
    NvOFGPUBufferHandle nvof_prev_buffer;
    NvOFGPUBufferHandle nvof_curr_buffer;
    NvOFGPUBufferHandle nvof_flow_buffer;
    NvOFGPUBufferHandle nvof_reverse_flow_buffer;
    CUdeviceptr nvof_prev_ptr;
    CUdeviceptr nvof_curr_ptr;
    CUdeviceptr nvof_flow_ptr;
    CUdeviceptr nvof_reverse_flow_ptr;
    uint32_t    nvof_prev_pitch;
    uint32_t    nvof_curr_pitch;
    uint32_t    nvof_flow_pitch;
    uint32_t    nvof_reverse_flow_pitch;
    int         nvof_grid;
    int         nvof_flow_w;
    int         nvof_flow_h;
    int         nvof_target_w;        // resolution NVOF was initialized at:
    int         nvof_target_h;        // source bin for Maxine, 4K for TRT post-upscale FRUC
    int         nvof_ready;           // 1 once nvof_init has succeeded for the graph stage dims
    // Source-resolution RGBA8 buffers for NVOF-before-upscaler. NVOF runs
    // on these (cheap, source-size pixel count), produces a half-time
    // interpolated source-size RGBA frame, which is then upscaled by
    // the same VSR pass that runs on the real source frame. Result:
    // VSR runs 2× per input frame but NVOF is ~9× cheaper.
    CUdeviceptr prev_pre_vsr_rgba;    // RGBA8 at source dims, previous decoded frame
    CUdeviceptr interp_pre_vsr_rgba;  // RGBA8 at source dims, FRUC interpolated half-time frame

    // Persistent NvCVImage views — SetImage stashes the pointer, so these
    // must outlive every NvVFX_Run call.
    NvCVImage   vsr_src_im;            // source-size RGBA — VSR input
    NvCVImage   vsr_dst_im;            // upscaled RGBA — VSR output

    // === TensorRT SR path (TRT SR), selected only from graph stages. ===
    // Engines are lazy-loaded on first use; we pick the one whose input
    // height is closest to the source. Output buffers vary per engine, so a
    // per-engine slot is allocated when that engine is selected.
    int           graph_model_family_code; // DPROC_MODEL_FAMILY_* for the active graph model.
    float         deband_strength;         // 0=off; >0 enables deband_4k kernel on the post-VSR RGBA8 4K frame
    float         temporal_denoise_strength; // 0=off; per-frame NVOF-warped prev-frame blend amount
    float         temporal_denoise_luma_max; // gate: skip pixels brighter than this luma; 0 disables, 0.6=all
    void*         trt_engine_480;              // TrtSrEngine* — 854×480   → 3416×1920
    void*         trt_engine_720;              // TrtSrEngine* — 1280×720  → 5120×2880
    void*         trt_engine_1080;             // TrtSrEngine* — 1920×1080 → 7680×4320
    int           trt_family_480;
    int           trt_family_720;
    int           trt_family_1080;
    // Per-bin TRT engine path. Populated from the JSON request — there is NO
    // env coupling. Empty = use the compiled-in default for that bin.
    char          trt_engine_path_480[1024];
    char          trt_engine_path_720[1024];
    char          trt_engine_path_1080[1024];
    // Active graph model input height. 480/720/1080 TRT bins are explicit graph caps.
    int           graph_model_input_h;
    // Where to drop the runtime_state JSON after engine load.
    char          runtime_state_path[1024];
    // Graph clock policy echoed into runtime_state so the dashboard can prove
    // the worker is using the same clock contract the launch graph declared.
    int           live_clock_mode;
    int           audio_pacing_mode;
    int           max_audio_lead_ms;
    int           max_av_delta_ms;
    // Audio passthrough — bypass Maxine ASR + EQ when 1.
    int           audio_passthrough_active;
    int           audio_maxine_active;
    CUdeviceptr   trt_in_chw_480;       // float32 NCHW [1,3,480,854]
    CUdeviceptr   trt_out_chw_480_a;    // slot A out buffer (ping-pong)
    CUdeviceptr   trt_out_chw_480_b;    // slot B out buffer
    CUdeviceptr   trt_in_chw_720;       // float32 NCHW [1,3,720,1280]
    CUdeviceptr   trt_out_chw_720_a;
    CUdeviceptr   trt_out_chw_720_b;
    CUdeviceptr   trt_in_chw_1080;      // float32 NCHW [1,3,1080,1920]
    CUdeviceptr   trt_out_chw_1080_a;
    CUdeviceptr   trt_out_chw_1080_b;
    // Slot rotation: which TRT output buffer the next enqueue writes to.
    int           trt_slot;             // 0 = A on next enqueue, 1 = B
    // Active engine for the current source dims.
    void*         trt_active;           // points at trt_engine_480 or trt_engine_720 or trt_engine_1080
    CUdeviceptr   trt_active_in_chw;
    CUdeviceptr   trt_active_out_chw_a; // slot A out buffer for active engine
    CUdeviceptr   trt_active_out_chw_b; // slot B out buffer for active engine
    CUdeviceptr   trt_active_out_chw;   // current frame's resolved slot pointer (a or b)
    int           trt_active_in_w;
    int           trt_active_in_h;
    int           trt_active_out_w;
    int           trt_active_out_h;
    DProcModelStage model_stages[DPROC_MAX_MODEL_STAGES];
    int           model_stage_count;
    char          d_pipeline_json[65536]; // Exact canonical graph loaded by this worker.
    CUdeviceptr   d_pipeline_rgba_a;
    CUdeviceptr   d_pipeline_rgba_b;
    size_t        d_pipeline_rgba_bytes;
} BakeWorker;

typedef struct {
    const char* url;             // upstream MKV/MP4 (atlas signed URL etc)
    double start_seconds;        // seek into upstream before decoding
    double duration_seconds;     // 0 = run until upstream EOF (open-ended);
                                 // >0 = exit after this many seconds of input PTS
                                 // (used by the HLS-segment endpoint, which
                                 // bounded single-shot segment jobs
                                 // of the player's playlist so VLC's seek bar
                                 // can show a real duration)
    const char* http_proxy;      // optional provider route proxy
    const char* user_agent;
    const char* headers;         // upstream-only HTTP headers, CRLF separated.
    const char* output_url;      // optional encoded packet sink; rtsp:// publishes directly
    const char* output_format;   // optional libavformat muxer name; default by output_url
    const char* control_path;    // optional hot-tuning JSON file, stderr telemetry only
    int is_live;                 // live demuxer hints even when URL is not .m3u8.
    // canon filter params
    float contrast;
    float saturation;
    float gamma;
    float cas_strength;          // post-VSR adaptive sharpen amount (0..1)
    float contrast_boost;        // post-VSR local-contrast / detail boost
                                 // (0..1). Edge-gated so flat areas untouched.
                                 // ~0.35 → "Sony Bravia" pop, 0 → off.
    float grain_strength;
    float temporal_strength;     // DLSAA-style temporal accumulation / anti-shimmer
    float edge_stability;        // edge-aware line stability gain for temporal reconstruction
    float audio_cleanup_strength; // Maxine sidechain cleanup intensity when AudioSR is active
    int audio_superres_mode;      // DPROC_AUDIO_SR_*; auto preserves 48 kHz stereo, ASR for narrowband
    int audio_passthrough;        // debug mode: decode source audio, AAC transport, no Maxine/ASR
    int audio_eq_mode;            // 0 flat, 1 auto, 2 voice, 3 movie, 4 music, 5 night
    int live_clock_mode;           // 0 source PTS, 1 provider-declared live PTS-gap preservation
    int audio_pacing_mode;         // DPROC_AUDIO_PACING_* from graph clockPolicy.audioPacingMode
    int max_audio_lead_ms;         // video-gated audio lead budget from graph clockPolicy
    int max_av_delta_ms;           // monitoring budget from graph clockPolicy
    const char* d_pipeline_json;   // canonical D-pipeline DSL echoed from Node/D graph
    DProcModelStage model_stages[DPROC_MAX_MODEL_STAGES];
    int model_stage_count;
    float deband_strength;         // 0=off, ~0.5=light, 1.0=strong. Sobel-gated dithered jitter on flat 4K regions.
    float custom_shader_intensity; // 0=off; manifest-selected custom CUDA post shader intensity.
    float temporal_denoise_strength; // 0=off; per-frame NVOF-warped previous-frame blend amount.
    float temporal_denoise_luma_max; // luma upper bound (0..1) for the dark-only gate; 0 disables, 0.6=all.
    // canon encoder params
    int bitrate_bps;
    int max_bitrate_bps;
    int live_output_cushion_ms; // graph encoderPolicy.outputQueueMs, explicit live packet cushion
    int fps;
    int audio_delay_ms;          // positive delays audio relative to video
    // Per-bin TRT engine paths (full path, no model_dir concat). Sent in
    // the spawn JSON; null/empty falls back to compiled-in defaults. Lets
    // the upscaler family change per-spawn without rebuilding the worker.
    // NEVER read from env.
    const char* trt_engine_path_480;
    const char* trt_engine_path_720;
    const char* trt_engine_path_1080;
    // Path the worker writes a runtime_state JSON snapshot to AFTER engine
    // load. Closes the panel↔worker contract loop: panel reads this file
    // to confirm what the worker actually loaded, and shows a red banner
    // if it differs from the panel pick. Empty = don't write.
    const char* runtime_state_path;
    // Resolved pipeline manifest JSON, per session. The api serializes the
    // ordered stage objects from src/pipeline/pipeline-manifest.js; bake.c
    // parses the ids and dimensions into BakeCtx.stages before the frame loop.
    const char* pipeline_manifest_json;
    // Separate resolved audio pipeline manifest. The audio lane is ordered
    // independently from the GPU/video lane but validated by the same compact
    // manifest parser before native streaming starts.
    const char* audio_pipeline_manifest_json;
    // mem2mem perf ring path. tmpfs file (e.g. /tmp/d-perf-<sess>.ring).
    // Worker mmaps + writes a 192-B PerfRingSlot per output frame. JS side
    // drains with positioned reads. Empty = disable perf-ring output.
    const char* perf_ring_path;
} BakeRequest;

typedef struct {
    int ok;
    char error[512];
    int frames_in;
    int frames_out;
    long long bytes_out;
    long long audio_packets;
    long long audio_frames;
    long long audio_maxine_runs;
    long long audio_encoded_packets;
    long long audio_bytes_out;
    double elapsed_seconds;
    double loop_seconds;
} BakeResult;

// Initialize a worker. Loads VSR effect, allocates GPU buffers, creates the
// CUDA stream. Returns 0 on success, non-zero on failure with error message
// available via bake_get_last_error().
int bake_worker_init(BakeWorker* w);
void bake_worker_destroy(BakeWorker* w);

// Stream one URL to an encoded sink until upstream EOF. Pipeline:
//   libavformat open URL + seek to start_seconds
//   decode loop: AVPacket → AVFrame (CUDA YUV420 via the codec-specific cuvid decoder)
//   per frame:
//     pre_vsr feeder kernel:  NV12/P010/P016 CUDA → selected input-bin RGBA8
//     graph upscaler:         Maxine VSR or TensorRT SR → normalized RGBA 4K
//     post finalize kernel:   RGBA8 4K → denoise/FXAA/dehalo/deband/tone/grain RGBA
//     graph-placed NVOF:      Maxine source-bin FRUC or TRT post-upscale 4K FRUC
//     temporal reconstruct:   final RGBA history → stable DLSAA-style edges
//     rgba_to_nv12 kernel:    final RGBA → NV12 CUDA 4K
//     hevc_nvenc:             NV12 CUDA → HEVC NAL units
//   per audio packet:
//     decode → source-aware audio lane:
//       full-band stereo → swr 48 kHz stereo → EQ/limiter → AAC transport PID
//       narrowband/forced → swr 16 kHz mono float → Maxine AudioSR16→48
//         sidechain over the 48 kHz stereo program bed → EQ/limiter → AAC
//   libavformat mux to RTSP or pipe:1 MPEG-TS (canon hvc1)
int bake_run(BakeWorker* w, const BakeRequest* req, BakeResult* res);

// Two-phase variants used by the prepared-pool path. Workers receive a
// {prep_only:true,url,...} JSON line at boot and call bake_prepare — which
// runs the avformat_open_input + find_stream_info + decoder open against
// the upstream URL and stashes the context internally. When a viewer attach
// finally arrives the worker gets a second JSON with start_seconds and
// calls bake_run_prepared, which only pays the av_seek_frame + open_output
// + decode→encode loop cost. Net: first-byte after seek ≈ 4 s instead of
// 8-9 s. If a prepared worker is killed before bake_run_prepared, the
// caller should invoke bake_release_prepared to close the stashed context.
int bake_prepare(BakeWorker* w, const BakeRequest* req, BakeResult* res);
int bake_run_prepared(BakeWorker* w, const BakeRequest* req, BakeResult* res);
void bake_release_prepared(BakeWorker* w);

// Last error string from any bake_* call. Thread-local. Owned by library.
const char* bake_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
