static double progress_log_interval_s(void) {
    const char* raw = getenv("DPROC_PROGRESS_LOG_INTERVAL_S");
    if (!raw || !*raw) return 30.0;
    char* end = NULL;
    double value = strtod(raw, &end);
    if (end == raw || !isfinite(value)) return 30.0;
    if (value < 0.0) return 0.0;
    if (value > 300.0) return 300.0;
    return value;
}

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

extern void launch_deband_4k_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W, float strength, uint32_t frame_seed, void* stream);

extern void launch_custom_shader_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W, float intensity, uint32_t frame_seed, void* stream);

extern void launch_rgba8_to_rgba8_resampled(
    const void* in_rgba,
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

extern void launch_rgb_chw_to_rgba8_resampled(
    const void* in_rgb_chw_fp32,
    void* out_rgba,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream);

#define STAGE_CHECK_CU(call, what) do { \
    CUresult _r = (call); \
    if (_r != CUDA_SUCCESS) { \
        snprintf(g_last_error, sizeof(g_last_error), "cuda_%s_err=%d", what, _r); \
        goto bail; \
    } \
} while (0)

#define STAGE_CHECK_NV(call, what) do { \
    NvCV_Status _s = (call); \
    if (_s != NVCV_SUCCESS) { \
        snprintf(g_last_error, sizeof(g_last_error), "vfx_%s_err=%d", what, _s); \
        goto bail; \
    } \
} while (0)

static double stage_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds) {
    if (seconds <= 0.0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
    if (ts.tv_nsec < 0) ts.tv_nsec = 0;
    if (ts.tv_nsec > 999999999L) ts.tv_nsec = 999999999L;
    while (nanosleep(&ts, &ts) != 0) {}
}

static void pace_live_output(BakeCtx* c, const BakeRequest* req, int64_t pts) {
    if (!c || !req || !req->is_live || c->out_fps_f <= 0.0 || pts < 0) return;
    const double now = stage_now_seconds();
    if (!c->live_pace_initialized) {
        c->live_pace_initialized = 1;
        c->live_pace_wall_start_s = now;
    }
    const double cushion_s = c->live_pace_cushion_s >= 0.0 ? c->live_pace_cushion_s : 0.750;
    const double video_s = (double)pts / c->out_fps_f;
    const double elapsed_s = now - c->live_pace_wall_start_s;
    const double allowed_s = elapsed_s + cushion_s;
    if (video_s <= allowed_s) return;
    const double sleep_s = fmin(video_s - allowed_s, 0.050);
    sleep_seconds(sleep_s);
    c->live_pace_slept_s += sleep_s;
    c->live_pace_sleep_count++;
}
