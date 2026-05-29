#ifndef DPROC_BAKE_INTERNAL_H
#define DPROC_BAKE_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <time.h>

#include <cuda.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/hwcontext.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "include/nvCVImage.h"
#include "include/nvVideoEffects.h"
#include "nvAudioEffects.h"
#include "bake.h"
#include "generated/d_pipeline_contract.h"
#include "pipeline_manifest.h"
#include "perf_ring.h"

typedef struct {
    NvAFX_Handle handle;
    unsigned int input_samples;
    unsigned int output_samples;
    unsigned int input_channels;
    unsigned int output_channels;
    unsigned int output_sample_rate;
    float* input_buffer;
    float* output_buffer;
    size_t input_buffer_samples;
    size_t output_buffer_samples;
    char model_dereverb_denoiser[PATH_MAX];
    char model_superres[PATH_MAX];
} MaxineAudioChain;

typedef struct {
    AVFormatContext* in_fmt;
    AVCodecContext*  dec_ctx;
    AVCodecContext*  enc_ctx;
    AVCodecContext*  audio_dec_ctx;
    AVCodecContext*  audio_enc_ctx;
    AVFormatContext* out_fmt;
    AVBufferRef*     hw_device_ref;
    AVBufferRef*     enc_frames_ref;
    AVFrame*         video_enc_frame;
    AVPacket*        video_enc_packet;
    SwrContext*      swr_ctx;
    SwrContext*      audio_bed_swr_ctx;
    AVAudioFifo*     audio_fifo;
    AVAudioFifo*     audio_bed_fifo;
    AVAudioFifo*     afx_input_fifo;
    MaxineAudioChain afx;
    int video_stream_idx;
    int out_video_stream_idx;
    int audio_stream_idx;
    int out_audio_stream_idx;
    int64_t audio_next_pts;
    int64_t audio_encode_limit_samples;
    int64_t audio_source_base_pts_us;
    int64_t audio_delay_samples;
    int audio_clock_initialized;
    long long audio_clock_resyncs;
    float audio_cleanup_strength;
    int audio_superres_mode;
    int audio_maxine_active;
    int audio_passthrough;
    float audio_auto_gain;
    float audio_balance_l;
    float audio_balance_r;
    float audio_bass_lp_l;
    float audio_bass_lp_r;
    int audio_eq_mode;
    int live_clock_mode;
    int audio_pacing_enabled;
    int max_audio_lead_ms;
    int max_av_delta_ms;
    int64_t max_audio_lead_samples;
    long long audio_auto_eq_blocks;
    int64_t video_sync_delay_pts;
    int64_t start_pts_us;
    double src_fps_f;
    double out_fps_f;
    int nvof_enabled;
    int cadence_lock;
    int live_pace_initialized;
    double live_pace_wall_start_s;
    double live_pace_cushion_s;
    double live_pace_slept_s;
    long long live_pace_sleep_count;
    int output_header_written;
    int first_frame_logged;
    double last_progress_log_s;
    double last_control_check_s;
    time_t control_mtime_sec;
    long control_mtime_nsec;
    long long audio_packets;
    long long audio_frames;
    long long audio_swr_samples;
    long long audio_maxine_runs;
    long long audio_maxine_in_samples;
    long long audio_maxine_out_samples;
    long long audio_encoded_packets;
    long long audio_bytes_out;
    long long cadence_drops;
    long long duplicated_frames;
    long long synthesized_frames;
    long long source_pts_jitter;
    long long source_pts_discontinuities;
    double stage_temporal_s;
    double stage_audio_s;
    double stage_pre_s;
    double stage_vsr_s;
    double stage_post_s;
    double stage_nvof_s;
    double stage_encode_s;
    DProcStage stages[DPROC_MAX_PIPELINE_STAGES];
    int       stage_count;
    DProcStage audio_stages[DPROC_MAX_PIPELINE_STAGES];
    int       audio_stage_count;
    PerfRing perf_ring;
    int      perf_ring_active;
} BakeCtx;

extern __thread char g_last_error[512];

int wrap_rgba_nvcv(NvCVImage* im, void* ptr, int W, int H);
int nvof_init(BakeWorker* w, int target_w, int target_h);
void nvof_destroy(BakeWorker* w);
int nvof_execute(BakeWorker* w, CUdeviceptr prev_rgba, CUdeviceptr curr_rgba);
int trt_upscaler_select_engine(BakeWorker* w, int src_w, int src_h);
int trt_upscaler_run_source_rgba_to(BakeWorker* w, CUdeviceptr source_rgba, CUdeviceptr out_rgba_4k);
int trt_stage_run_rgba_to_chw(BakeWorker* w, DProcModelStage* stage,
                              CUdeviceptr source_rgba, int src_w, int src_h,
                              CUdeviceptr* out_chw, int* out_w, int* out_h);
int trt_stage_run_chw_to_chw(BakeWorker* w, DProcModelStage* stage,
                             CUdeviceptr in_chw, int in_w, int in_h,
                             CUdeviceptr* out_chw, int* out_w, int* out_h);

int pipeline_stage_index(const BakeCtx* c, uint32_t id);
int pipeline_stage_enabled(const BakeCtx* c, uint32_t id);
int run_source_prep_stage(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                          AVFrame* in_frame, int input_format);
int run_source_nvof_stage(BakeCtx* c, BakeWorker* w);
int run_output_nvof_stage(BakeCtx* c, BakeWorker* w);
int run_d_model_pipeline_range_to_rgba(BakeCtx* c, BakeWorker* w,
                                       CUdeviceptr source_rgba,
                                       int source_w, int source_h,
                                       int start_index, int end_index,
                                       CUdeviceptr out_rgba,
                                       int out_w, int out_h);
int run_upscaler_stage_from_model_index(BakeCtx* c, BakeWorker* w,
                                        CUdeviceptr source_rgba,
                                        int source_w, int source_h,
                                        int start_index);
int run_manifest_render_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                              CUdeviceptr source_rgba,
                              int frame_seed, int have_prev_final,
                              int* flow_ready_io);
int run_manifest_encode_rgba_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                                   CUdeviceptr rgba, int64_t pts,
                                   int* n_out, long long* bytes_out);
int run_manifest_output_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                              CUdeviceptr source_rgba, int64_t pts,
                              int frame_seed, int have_prev_final,
                              int flow_ready,
                              int* n_out, long long* bytes_out);
int run_manifest_output_frame_from_model_index(BakeCtx* c, BakeWorker* w,
                              const BakeRequest* req,
                              CUdeviceptr source_rgba, int source_w, int source_h,
                              int start_index, int64_t pts,
                              int frame_seed, int have_prev_final,
                              int flow_ready,
                              int* n_out, long long* bytes_out);
void maybe_log_pipeline_progress(BakeCtx* c,
                                 int n_in,
                                 int n_out,
                                 long long bytes_out,
                                 int64_t in_pts_us,
                                 double loop_started_s);

#endif
