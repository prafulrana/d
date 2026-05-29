int bake_worker_init(BakeWorker* w) {
    memset(w, 0, sizeof(*w));
    g_last_error[0] = 0;

    // Pre-load nvvfx feature plugin chain with RTLD_GLOBAL — matches Python loader.
    static const char* preload[] = {
        "libnppc.so.12",
        "libnppial.so.12", "libnppicc.so.12", "libnppidei.so.12",
        "libnppif.so.12", "libnppig.so.12", "libnppim.so.12",
        "libnppist.so.12", "libnppitc.so.12",
        "libcudnn.so.9",
        "libnvinfer.so.10", "libnvinfer_plugin.so.10", "libnvonnxparser.so.10",
        "libNVCVImage.so",
        "libnvngxruntime.so",
        "libVideoFXLocal.so",
        "libVideoFX.so",
        "libnvVFXVideoSuperRes.so",
        "libnvidia-ngx-vsr.so.1.8.2",
        NULL,
    };
    for (int i = 0; preload[i]; i++) {
        if (!dlopen(preload[i], RTLD_NOW | RTLD_GLOBAL)) {
            const char* name = preload[i];
            if (strstr(name, "VideoFX") || strstr(name, "VSR") || strstr(name, "ngx")) {
                BAIL_FMT("dlopen_%s_failed=%.200s", name, dlerror());
            }
            dlerror();
        }
    }

    CHECK_CU(cuInit(0), "init");
    CUdevice dev;
    CHECK_CU(cuDeviceGet(&dev, 0), "deviceGet");
    (void)cuDevicePrimaryCtxSetFlags(dev, CU_CTX_SCHED_BLOCKING_SYNC);
    CHECK_CU(cuDevicePrimaryCtxRetain(&w->cu_ctx, dev), "primaryCtxRetain");
    CHECK_CU(cuCtxSetCurrent(w->cu_ctx), "ctxSetCurrent");
    CHECK_CU(cuStreamCreate(&w->cu_stream, CU_STREAM_NON_BLOCKING), "streamCreate");
    // Phase-2 async: stream B carries the downstream chain (resample → post →
    // temporal → FRUC → rgba_to_nv12). ev_trt_done signals end-of-TRT-upscaler on
    // stream A; stream B waits on it before reading the slot's output buffer.
    // While B drains frame N, stream A starts TRT upscaler N+1 → end-to-end wall
    // time is MAX(trt_upscaler, downstream) instead of SUM.
    CHECK_CU(cuStreamCreate(&w->cu_stream_b, CU_STREAM_NON_BLOCKING), "streamCreateB");
    CHECK_CU(cuEventCreate(&w->ev_trt_done, CU_EVENT_DISABLE_TIMING), "eventCreate");
    CHECK_CU(cuEventCreate(&w->ev_vsr_done, CU_EVENT_DISABLE_TIMING), "eventCreateVsr");
    CHECK_CU(cuEventCreate(&w->ev_nvof_done, CU_EVENT_DISABLE_TIMING), "eventCreateNvof");
    CHECK_CU(cuEventCreate(&w->ev_encode_ready, CU_EVENT_DISABLE_TIMING), "eventCreateEncode");

    w->vsr_out_bytes = (size_t)BAKE_OUTPUT_WIDTH * BAKE_OUTPUT_HEIGHT * 4;
    CHECK_CU(cuMemAlloc(&w->vsr_out_rgba, w->vsr_out_bytes), "allocVsrOut");
    CHECK_CU(cuMemAlloc(&w->maxine_raw_rgba, w->vsr_out_bytes), "allocMaxineRaw");
    CHECK_CU(cuMemAlloc(&w->final_rgba, w->vsr_out_bytes), "allocFinalRgba");
    CHECK_CU(cuMemAlloc(&w->prev_final_rgba, w->vsr_out_bytes), "allocPrevFinalRgba");
    CHECK_CU(cuMemAlloc(&w->interp_final_rgba, w->vsr_out_bytes), "allocInterpFinalRgba");
    // Model selection is entirely JSON-graph-driven; no env coupling and no
    // secondary enable flags.
    w->worker_ready = 1;
    fprintf(stderr, "[d_native_processor] native runtime ready: output=%dx%d filters=post4k+fruc (model from graph)\n",
            BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT);
    return 0;
bail:
    bake_worker_destroy(w);
    return -1;
}

void bake_worker_destroy(BakeWorker* w) {
    if (!w) return;
    nvof_destroy(w);
    destroy_trt_upscaler_engines(w);
    destroy_vsr_effect(w);
    if (w->d_pipeline_rgba_b) { cuMemFree(w->d_pipeline_rgba_b); w->d_pipeline_rgba_b = 0; }
    if (w->d_pipeline_rgba_a) { cuMemFree(w->d_pipeline_rgba_a); w->d_pipeline_rgba_a = 0; }
    w->d_pipeline_rgba_bytes = 0;
    if (w->interp_final_rgba) { cuMemFree(w->interp_final_rgba); w->interp_final_rgba = 0; }
    if (w->prev_final_rgba) { cuMemFree(w->prev_final_rgba); w->prev_final_rgba = 0; }
    if (w->final_rgba) { cuMemFree(w->final_rgba); w->final_rgba = 0; }
    if (w->maxine_raw_rgba) { cuMemFree(w->maxine_raw_rgba); w->maxine_raw_rgba = 0; }
    if (w->vsr_out_rgba) { cuMemFree(w->vsr_out_rgba); w->vsr_out_rgba = 0; }
    if (w->ev_vsr_done) { cuEventDestroy(w->ev_vsr_done); w->ev_vsr_done = NULL; }
    if (w->ev_nvof_done) { cuEventDestroy(w->ev_nvof_done); w->ev_nvof_done = NULL; }
    if (w->ev_encode_ready) { cuEventDestroy(w->ev_encode_ready); w->ev_encode_ready = NULL; }
    if (w->ev_trt_done) { cuEventDestroy(w->ev_trt_done); w->ev_trt_done = NULL; }
    if (w->cu_stream_b) { cuStreamDestroy(w->cu_stream_b); w->cu_stream_b = NULL; }
    if (w->cu_stream)   { cuStreamDestroy(w->cu_stream);   w->cu_stream   = NULL; }
    if (w->cu_ctx) { CUdevice dev; cuDeviceGet(&dev, 0); cuDevicePrimaryCtxRelease(dev); w->cu_ctx = NULL; }
    w->worker_ready = 0;
}

static int ensure_worker_ready(BakeWorker* w) {
    if (!w) {
        snprintf(g_last_error, sizeof(g_last_error), "worker_null");
        return -1;
    }
    if (w->worker_ready) return 0;
    if (bake_worker_init(w) != 0) return -1;
    return 0;
}

// ----------------------------------------------------------------------------
// Per-bake state
// ----------------------------------------------------------------------------

#define DPROC_AFX_DEFAULT_ROOT "/opt/dgst/src/native/maxine-audio"
#define DPROC_AFX_SM_DEFAULT "sm_120"
#define DPROC_AFX_INPUT_RATE 16000
#define DPROC_AFX_OUTPUT_RATE 48000
#define DPROC_AFX_INPUT_CHANNELS 1
#define DPROC_AFX_OUTPUT_CHANNELS 2

static int open_audio_decoder(BakeCtx* c);
static int init_maxine_audio_chain(BakeCtx* c);
static int set_maxine_audio_cleanup_strength(BakeCtx* c, float cleanup_strength, int fatal);
static void destroy_maxine_audio_chain(MaxineAudioChain* chain);
static int encode_audio_fifo(BakeCtx* c, long long* bytes_out, int flush);
static int process_maxine_audio_fifo(BakeCtx* c, long long* bytes_out, int flush);
static int process_audio_packet(BakeCtx* c, AVPacket* pkt, long long* bytes_out);
static int flush_audio(BakeCtx* c, long long* bytes_out);
static void apply_audio_auto_mix_guard(BakeCtx* c, float* left, float* right, int samples);

static int d_model_family_to_graph_model_family_code(int family_code) {
    if (family_code == DPROC_MODEL_FAMILY_MAXINE) return DPROC_MODEL_FAMILY_MAXINE;
    if (family_code == DPROC_MODEL_FAMILY_ESRGAN) return DPROC_MODEL_FAMILY_ESRGAN;
    if (family_code == DPROC_MODEL_FAMILY_CUGAN) return DPROC_MODEL_FAMILY_CUGAN;
    return -1;
}

static const char* d_model_default_engine_path(const DProcModelStage* s) {
    if (!s) return "";
    const int bin = s->input_h;
    if (s->family_code == DPROC_MODEL_FAMILY_CUGAN) {
        if (bin == 480) return "/opt/dgst/models/upscalers/cugan-pro-up4x_480p_fp16.engine";
        if (bin == 720) return "/opt/dgst/models/upscalers/cugan-pro-up4x_720p_fp16.engine";
        if (bin == 1080) return "/opt/dgst/models/upscalers/cugan-pro-up4x_1080p_fp16.engine";
    }
    if (s->family_code == DPROC_MODEL_FAMILY_ESRGAN) {
        if (bin == 480) return "/opt/dgst/models/upscalers/realesr-general-x4v3_480p_fp16.engine";
        if (bin == 720) return "/opt/dgst/models/upscalers/realesr-general-x4v3_720p_fp16.engine";
        if (bin == 1080) return "/opt/dgst/models/upscalers/realesr-general-x4v3_1080p_fp16.engine";
    }
    return "";
}

static void d_model_copy_engine_path_to_worker(BakeWorker* w, DProcModelStage* s) {
    if (!w || !s) return;
    const char* path = s->engine_path[0] ? s->engine_path : d_model_default_engine_path(s);
    if (!path) path = "";
    if (!s->engine_path[0] && path[0]) snprintf(s->engine_path, sizeof(s->engine_path), "%s", path);
    if (s->input_h == 480) {
        snprintf(w->trt_engine_path_480, sizeof(w->trt_engine_path_480), "%s", path);
    } else if (s->input_h == 720) {
        snprintf(w->trt_engine_path_720, sizeof(w->trt_engine_path_720), "%s", path);
    } else if (s->input_h == 1080) {
        snprintf(w->trt_engine_path_1080, sizeof(w->trt_engine_path_1080), "%s", path);
    }
}

static int ensure_d_pipeline_rgba_buffers(BakeWorker* w, int width, int height) {
    if (!w) return -1;
    const size_t bytes = (size_t)width * (size_t)height * 4;
    if (bytes == 0) {
        snprintf(g_last_error, sizeof(g_last_error), "d_pipeline_bad_rgba_dims=%dx%d", width, height);
        return -1;
    }
    if (w->d_pipeline_rgba_a && w->d_pipeline_rgba_b && w->d_pipeline_rgba_bytes >= bytes) return 0;
    if (w->d_pipeline_rgba_b) { cuMemFree(w->d_pipeline_rgba_b); w->d_pipeline_rgba_b = 0; }
    if (w->d_pipeline_rgba_a) { cuMemFree(w->d_pipeline_rgba_a); w->d_pipeline_rgba_a = 0; }
    CHECK_CU(cuMemAlloc(&w->d_pipeline_rgba_a, bytes), "allocDPipelineRgbaA");
    CHECK_CU(cuMemAlloc(&w->d_pipeline_rgba_b, bytes), "allocDPipelineRgbaB");
    w->d_pipeline_rgba_bytes = bytes;
    return 0;
bail:
    if (w->d_pipeline_rgba_b) { cuMemFree(w->d_pipeline_rgba_b); w->d_pipeline_rgba_b = 0; }
    if (w->d_pipeline_rgba_a) { cuMemFree(w->d_pipeline_rgba_a); w->d_pipeline_rgba_a = 0; }
    w->d_pipeline_rgba_bytes = 0;
    return -1;
}

static int configure_d_model_pipeline_for_input(BakeWorker* w, int prep_w, int prep_h) {
    if (!w || !w->worker_ready) {
        snprintf(g_last_error, sizeof(g_last_error), "worker_not_ready");
        return -1;
    }
    const int saved_mode = w->graph_model_family_code;
    const int saved_bin = w->graph_model_input_h;
    if (w->model_stage_count <= 0) BAIL("d_pipeline_model_stages_missing");
    const DProcModelStage* first = &w->model_stages[0];
    if (first->input_w > 0 && first->input_h > 0) {
        prep_w = first->input_w;
        prep_h = first->input_h;
    }
    if (ensure_source_rgba_buffers(w, prep_w, prep_h) < 0) return -1;

    int max_rgba_w = 0;
    int max_rgba_h = 0;
    for (int i = 0; i < w->model_stage_count; i++) {
        DProcModelStage* s = &w->model_stages[i];
        if (s->kind_code == DPROC_MODEL_KIND_MODEL && s->engine_code == DPROC_MODEL_ENGINE_TRT && s->pass_through) {
            s->ready = 1;
            if (s->output_w > max_rgba_w) max_rgba_w = s->output_w;
            if (s->output_h > max_rgba_h) max_rgba_h = s->output_h;
            fprintf(stderr,
                    "[d_native_processor] d_model_stage ready index=%d id=%s family=%s engine=tensorrt pass_through=1 input=%dx%d@%d output=%dx%d@%d path=%s\n",
                    i, s->id, s->family[0] ? s->family : "custom",
                    s->input_w, s->input_h, s->input_fps,
                    s->output_w, s->output_h, s->output_fps,
                    s->engine_path);
        } else if (s->kind_code == DPROC_MODEL_KIND_MODEL && s->engine_code == DPROC_MODEL_ENGINE_TRT) {
            const int mode = d_model_family_to_graph_model_family_code(s->family_code);
            if (mode < 0) BAIL_FMT("d_pipeline_bad_trt_family id=%s", s->id);
            d_model_copy_engine_path_to_worker(w, s);
            w->graph_model_family_code = mode;
            w->graph_model_input_h = s->input_h;
            if (trt_upscaler_select_engine(w, prep_w, prep_h) < 0) goto bail;
            s->ready = 1;
            fprintf(stderr,
                    "[d_native_processor] d_model_stage ready index=%d id=%s family=%s engine=tensorrt input=%dx%d output_internal=%dx%d declared_output=%dx%d@%d path=%s\n",
                    i, s->id, dproc_model_family_name(s->family_code),
                    s->input_w, s->input_h,
                    w->trt_active_out_w, w->trt_active_out_h,
                    s->output_w, s->output_h, s->output_fps,
                    s->engine_path);
        } else if (s->kind_code == DPROC_MODEL_KIND_MODEL && s->engine_code == DPROC_MODEL_ENGINE_MAXINE) {
            if (ensure_d_pipeline_rgba_buffers(w, s->input_w, s->input_h) < 0) goto bail;
            if (configure_maxine_effect_for_input(w, s->input_w, s->input_h) < 0) goto bail;
            s->ready = 1;
            if (s->input_w > max_rgba_w) max_rgba_w = s->input_w;
            if (s->input_h > max_rgba_h) max_rgba_h = s->input_h;
            fprintf(stderr,
                    "[d_native_processor] d_model_stage ready index=%d id=%s family=maxine input=%dx%d@%d output=%dx%d@%d\n",
                    i, s->id, s->input_w, s->input_h, s->input_fps,
                    s->output_w, s->output_h, s->output_fps);
        } else if (s->kind_code == DPROC_MODEL_KIND_MOTION || s->kind_code == DPROC_MODEL_KIND_CUDA) {
            s->ready = 1;
            if (s->output_w > max_rgba_w) max_rgba_w = s->output_w;
            if (s->output_h > max_rgba_h) max_rgba_h = s->output_h;
            fprintf(stderr,
                    "[d_native_processor] d_model_stage ready index=%d id=%s kind=%s engine=%s input=%dx%d@%d output=%dx%d@%d\n",
                    i, s->id, s->kind, dproc_model_engine_name(s->engine_code),
                    s->input_w, s->input_h, s->input_fps,
                    s->output_w, s->output_h, s->output_fps);
        }
    }
    w->graph_model_family_code = saved_mode;
    w->graph_model_input_h = saved_bin;
    if (max_rgba_w > 0 && max_rgba_h > 0 && ensure_d_pipeline_rgba_buffers(w, max_rgba_w, max_rgba_h) < 0) goto bail;
    bake_write_runtime_state(w, prep_w, prep_h, first->input_h);
    return 0;
bail:
    w->graph_model_family_code = saved_mode;
    w->graph_model_input_h = saved_bin;
    return -1;
}

static int configure_vsr_for_ctx(BakeWorker* w, BakeCtx* c) {
    if (!c || !c->in_fmt || c->video_stream_idx < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "vsr_ctx_missing_video");
        return -1;
    }
    AVCodecParameters* p = c->in_fmt->streams[c->video_stream_idx]->codecpar;
    int input_w = p->width;
    int input_h = p->height;
    int manifest_w = 0;
    int manifest_h = 0;
    if (dproc_stage_dims_for(c->stages, c->stage_count, STAGE_NV12_TO_RGB_CHW,
                            &manifest_w, &manifest_h) == 0 &&
        manifest_w > 0 && manifest_h > 0) {
        input_w = manifest_w;
        input_h = manifest_h;
    }
    if (w && w->model_stage_count > 0) {
        return configure_d_model_pipeline_for_input(w, input_w, input_h);
    }
    return configure_vsr_for_input(w, input_w, input_h);
}

// ----------------------------------------------------------------------------
// TensorRT SR stages are selected only from model stages in the loaded graph.
// Fixed-shape engines lazy-load on first use; the selected engine declares its
// own graph input/output caps, and the downstream CUDA adapters normalize to
// the final output caps declared by the graph.
// ----------------------------------------------------------------------------
