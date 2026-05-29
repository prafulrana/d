#define DPROC_TRT_SR_480_W 854
#define DPROC_TRT_SR_480_H 480
#define DPROC_TRT_SR_480_OUT_W 3416
#define DPROC_TRT_SR_480_OUT_H 1920
#define DPROC_TRT_SR_720_W 1280
#define DPROC_TRT_SR_720_H 720
#define DPROC_TRT_SR_720_OUT_W 5120
#define DPROC_TRT_SR_720_OUT_H 2880
#define DPROC_TRT_SR_1080_W 1920
#define DPROC_TRT_SR_1080_H 1080
#define DPROC_TRT_SR_1080_OUT_W 7680
#define DPROC_TRT_SR_1080_OUT_H 4320
#define DPROC_TRT_SR_DEFAULT_PATH_480  "/opt/dgst/models/upscalers/realesr-general-x4v3_480p_fp16.engine"
#define DPROC_TRT_SR_DEFAULT_PATH_720  "/opt/dgst/models/upscalers/realesr-general-x4v3_720p_fp16.engine"
#define DPROC_TRT_SR_DEFAULT_PATH_1080 "/opt/dgst/models/upscalers/realesr-general-x4v3_1080p_fp16.engine"

// Engine path resolution: pure JSON-config. The full path was sent in the
// spawn JSON request and copied onto BakeWorker; we read it from there.
// NO env reads. If the worker field is empty, fall back to the compiled-in
// default for that bin.
static const char* trt_engine_path(BakeWorker* w, int height_bin) {
    if (w) {
        if (height_bin == 480  && w->trt_engine_path_480[0])  return w->trt_engine_path_480;
        if (height_bin == 720  && w->trt_engine_path_720[0])  return w->trt_engine_path_720;
        if (height_bin == 1080 && w->trt_engine_path_1080[0]) return w->trt_engine_path_1080;
    }
    if (height_bin == 480)  return DPROC_TRT_SR_DEFAULT_PATH_480;
    if (height_bin == 720)  return DPROC_TRT_SR_DEFAULT_PATH_720;
    if (height_bin == 1080) return DPROC_TRT_SR_DEFAULT_PATH_1080;
    return DPROC_TRT_SR_DEFAULT_PATH_720;
}

static int validate_trt_engine_family(BakeWorker* w, int bin) {
    const char* path = trt_engine_path(w, bin);
    const int path_is_cugan = path && strstr(path, "cugan") != NULL;
    if (w->graph_model_family_code == DPROC_MODEL_FAMILY_CUGAN && !path_is_cugan) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "trt_sr_engine_family_mismatch=expected=%s bin_%d path=%.256s",
                 dproc_model_family_name(w->graph_model_family_code), bin, path ? path : "");
        return -1;
    }
    if (w->graph_model_family_code == DPROC_MODEL_FAMILY_ESRGAN && path_is_cugan) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "trt_sr_engine_family_mismatch=expected=%s bin_%d path=%.256s",
                 dproc_model_family_name(w->graph_model_family_code), bin, path ? path : "");
        return -1;
    }
    return 0;
}

static int alloc_trt_upscaler_engine_480(BakeWorker* w) {
    if (w->trt_engine_480) return 0;
    char err[256] = {0};
    char path[1024];
    snprintf(path, sizeof(path), "%s", trt_engine_path(w, 480));
    TrtSrEngine* r = trt_sr_engine_create(path,
                                       DPROC_TRT_SR_480_W, DPROC_TRT_SR_480_H,
                                       DPROC_TRT_SR_480_OUT_W, DPROC_TRT_SR_480_OUT_H,
                                       w->cu_stream, err);
    if (!r) BAIL_FMT("trt_engine_480_load=%s", err);
    const size_t in_bytes = (size_t)DPROC_TRT_SR_480_W * DPROC_TRT_SR_480_H * 3 * sizeof(float);
    const size_t out_bytes = (size_t)DPROC_TRT_SR_480_OUT_W * DPROC_TRT_SR_480_OUT_H * 3 * sizeof(float);
    CHECK_CU(cuMemAlloc(&w->trt_in_chw_480, in_bytes), "trtAlloc480In");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_480_a, out_bytes), "trtAlloc480OutA");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_480_b, out_bytes), "trtAlloc480OutB");
    // Zero + warm up engine on slot A — JIT codegen happens here so first
    // real frame is steady-state.
    CHECK_CU(cuMemsetD8(w->trt_in_chw_480, 0, in_bytes), "trtZero480In");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_480_a, 0, out_bytes), "trtZero480OutA");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_480_b, 0, out_bytes), "trtZero480OutB");
    if (trt_sr_engine_run(r, w->trt_in_chw_480, w->trt_out_chw_480_a, err) < 0) {
        trt_sr_engine_destroy(r);
        BAIL_FMT("trt_engine_480_warmup=%s", err);
    }
    w->trt_engine_480 = r;
    fprintf(stderr, "[d_native_processor] TRT SR 480p engine loaded+warmed path=%s in=%zu out_per_slot=%zu (×2 slots) devmem=%zu\n",
            path, in_bytes, out_bytes, trt_sr_engine_device_memory_bytes(r));
    return 0;
bail:
    return -1;
}

static int alloc_trt_upscaler_engine_1080(BakeWorker* w) {
    if (w->trt_engine_1080) return 0;
    char err[256] = {0};
    char path[1024];
    snprintf(path, sizeof(path), "%s", trt_engine_path(w, 1080));
    TrtSrEngine* r = trt_sr_engine_create(path,
                                       DPROC_TRT_SR_1080_W, DPROC_TRT_SR_1080_H,
                                       DPROC_TRT_SR_1080_OUT_W, DPROC_TRT_SR_1080_OUT_H,
                                       w->cu_stream, err);
    if (!r) BAIL_FMT("trt_engine_1080_load=%s", err);
    const size_t in_bytes = (size_t)DPROC_TRT_SR_1080_W * DPROC_TRT_SR_1080_H * 3 * sizeof(float);
    const size_t out_bytes = (size_t)DPROC_TRT_SR_1080_OUT_W * DPROC_TRT_SR_1080_OUT_H * 3 * sizeof(float);
    CHECK_CU(cuMemAlloc(&w->trt_in_chw_1080, in_bytes), "trtAlloc1080In");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_1080_a, out_bytes), "trtAlloc1080OutA");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_1080_b, out_bytes), "trtAlloc1080OutB");
    CHECK_CU(cuMemsetD8(w->trt_in_chw_1080, 0, in_bytes), "trtZero1080In");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_1080_a, 0, out_bytes), "trtZero1080OutA");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_1080_b, 0, out_bytes), "trtZero1080OutB");
    if (trt_sr_engine_run(r, w->trt_in_chw_1080, w->trt_out_chw_1080_a, err) < 0) {
        trt_sr_engine_destroy(r);
        BAIL_FMT("trt_engine_1080_warmup=%s", err);
    }
    w->trt_engine_1080 = r;
    fprintf(stderr, "[d_native_processor] TRT SR 1080p engine loaded+warmed path=%s in=%zu out_per_slot=%zu (×2 slots) devmem=%zu\n",
            path, in_bytes, out_bytes, trt_sr_engine_device_memory_bytes(r));
    return 0;
bail:
    return -1;
}

static int alloc_trt_upscaler_engine_720(BakeWorker* w) {
    if (w->trt_engine_720) return 0;
    char err[256] = {0};
    char path[1024];
    snprintf(path, sizeof(path), "%s", trt_engine_path(w, 720));
    TrtSrEngine* r = trt_sr_engine_create(path,
                                       DPROC_TRT_SR_720_W, DPROC_TRT_SR_720_H,
                                       DPROC_TRT_SR_720_OUT_W, DPROC_TRT_SR_720_OUT_H,
                                       w->cu_stream, err);
    if (!r) BAIL_FMT("trt_engine_720_load=%s", err);
    const size_t in_bytes = (size_t)DPROC_TRT_SR_720_W * DPROC_TRT_SR_720_H * 3 * sizeof(float);
    const size_t out_bytes = (size_t)DPROC_TRT_SR_720_OUT_W * DPROC_TRT_SR_720_OUT_H * 3 * sizeof(float);
    CHECK_CU(cuMemAlloc(&w->trt_in_chw_720, in_bytes), "trtAlloc720In");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_720_a, out_bytes), "trtAlloc720OutA");
    CHECK_CU(cuMemAlloc(&w->trt_out_chw_720_b, out_bytes), "trtAlloc720OutB");
    CHECK_CU(cuMemsetD8(w->trt_in_chw_720, 0, in_bytes), "trtZero720In");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_720_a, 0, out_bytes), "trtZero720OutA");
    CHECK_CU(cuMemsetD8(w->trt_out_chw_720_b, 0, out_bytes), "trtZero720OutB");
    if (trt_sr_engine_run(r, w->trt_in_chw_720, w->trt_out_chw_720_a, err) < 0) {
        trt_sr_engine_destroy(r);
        BAIL_FMT("trt_engine_720_warmup=%s", err);
    }
    w->trt_engine_720 = r;
    fprintf(stderr, "[d_native_processor] TRT SR 720p engine loaded+warmed path=%s in=%zu out_per_slot=%zu (×2 slots) devmem=%zu\n",
            path, in_bytes, out_bytes, trt_sr_engine_device_memory_bytes(r));
    return 0;
bail:
    return -1;
}

// One bin-load helper. Per-bin allocation + slot wiring lives in one
// place; the bin number is the only thing that varies. Kills the 3x
// repetition of forced-bin branches.
static int trt_upscaler_load_bin(BakeWorker* w, int bin, int src_w, int src_h) {
    (void)src_w;
    (void)src_h;
    if (validate_trt_engine_family(w, bin) < 0) return -1;
    void* engine = NULL;
    CUdeviceptr in_chw = 0, out_a = 0, out_b = 0;
    int in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    int alloc_rc = 0;
    int* family_slot = NULL;
    switch (bin) {
        case 480:
            family_slot = &w->trt_family_480;
            if (*family_slot && *family_slot != w->graph_model_family_code) {
                snprintf(g_last_error, sizeof(g_last_error), "trt_bin_family_collision=480 loaded=%d requested=%d",
                         *family_slot, w->graph_model_family_code);
                return -1;
            }
            alloc_rc = alloc_trt_upscaler_engine_480(w);
            engine = w->trt_engine_480; in_chw = w->trt_in_chw_480;
            out_a = w->trt_out_chw_480_a; out_b = w->trt_out_chw_480_b;
            in_w = DPROC_TRT_SR_480_W;  in_h = DPROC_TRT_SR_480_H;
            out_w = DPROC_TRT_SR_480_OUT_W; out_h = DPROC_TRT_SR_480_OUT_H;
            break;
        case 720:
            family_slot = &w->trt_family_720;
            if (*family_slot && *family_slot != w->graph_model_family_code) {
                snprintf(g_last_error, sizeof(g_last_error), "trt_bin_family_collision=720 loaded=%d requested=%d",
                         *family_slot, w->graph_model_family_code);
                return -1;
            }
            alloc_rc = alloc_trt_upscaler_engine_720(w);
            engine = w->trt_engine_720; in_chw = w->trt_in_chw_720;
            out_a = w->trt_out_chw_720_a; out_b = w->trt_out_chw_720_b;
            in_w = DPROC_TRT_SR_720_W;  in_h = DPROC_TRT_SR_720_H;
            out_w = DPROC_TRT_SR_720_OUT_W; out_h = DPROC_TRT_SR_720_OUT_H;
            break;
        case 1080:
            family_slot = &w->trt_family_1080;
            if (*family_slot && *family_slot != w->graph_model_family_code) {
                snprintf(g_last_error, sizeof(g_last_error), "trt_bin_family_collision=1080 loaded=%d requested=%d",
                         *family_slot, w->graph_model_family_code);
                return -1;
            }
            alloc_rc = alloc_trt_upscaler_engine_1080(w);
            engine = w->trt_engine_1080; in_chw = w->trt_in_chw_1080;
            out_a = w->trt_out_chw_1080_a; out_b = w->trt_out_chw_1080_b;
            in_w = DPROC_TRT_SR_1080_W;  in_h = DPROC_TRT_SR_1080_H;
            out_w = DPROC_TRT_SR_1080_OUT_W; out_h = DPROC_TRT_SR_1080_OUT_H;
            break;
        default:
            snprintf(g_last_error, sizeof(g_last_error), "trt_upscaler_bin_unknown=%d", bin);
            return -1;
    }
    if (alloc_rc < 0) {
        // Fail LOUD per the framework contract: the panel asked for this
        // bin, the engine couldn't load. The worker exits, the panel sees
        // a runtime_state mismatch + red banner instead of a silent
        // fallback to a different bin.
        snprintf(g_last_error, sizeof(g_last_error), "trt_engine_missing_or_corrupt=bin_%d path=%.256s",
                 bin, trt_engine_path(w, bin));
        return -1;
    }
    w->trt_active = engine;
    w->trt_active_in_chw = in_chw;
    w->trt_active_out_chw_a = out_a;
    w->trt_active_out_chw_b = out_b;
    w->trt_active_in_w = in_w;   w->trt_active_in_h = in_h;
    w->trt_active_out_w = out_w; w->trt_active_out_h = out_h;
    w->trt_active_out_chw = out_a;
    w->trt_slot = 0;
    if (family_slot) *family_slot = w->graph_model_family_code;
    return 0;
}

// Engine bin selection. The graph stage input caps select the TRT binding.
// The CUDA preprocess kernel resamples the source to that explicit shape.
int trt_upscaler_select_engine(BakeWorker* w, int src_w, int src_h) {
    if (!w) return 0;
    if (w->graph_model_family_code == DPROC_MODEL_FAMILY_MAXINE) return 0;
    if (w->graph_model_family_code != DPROC_MODEL_FAMILY_ESRGAN && w->graph_model_family_code != DPROC_MODEL_FAMILY_CUGAN) {
        snprintf(g_last_error, sizeof(g_last_error), "bad_trt_graph_model_family_code=%d", w->graph_model_family_code);
        return -1;
    }
    if (w->graph_model_input_h == 480 || w->graph_model_input_h == 720 || w->graph_model_input_h == 1080) {
        return trt_upscaler_load_bin(w, w->graph_model_input_h, src_w, src_h);
    }
    snprintf(g_last_error, sizeof(g_last_error), "missing_graph_model_input_h");
    return -1;
}

static int trt_stage_family_to_graph_model_family_code(int family_code) {
    if (family_code == DPROC_MODEL_FAMILY_ESRGAN) return DPROC_MODEL_FAMILY_ESRGAN;
    if (family_code == DPROC_MODEL_FAMILY_CUGAN) return DPROC_MODEL_FAMILY_CUGAN;
    return -1;
}

static void trt_stage_copy_path(BakeWorker* w, const DProcModelStage* stage) {
    if (!w || !stage || !stage->engine_path[0]) return;
    if (stage->input_h == 480) {
        snprintf(w->trt_engine_path_480, sizeof(w->trt_engine_path_480), "%s", stage->engine_path);
    } else if (stage->input_h == 720) {
        snprintf(w->trt_engine_path_720, sizeof(w->trt_engine_path_720), "%s", stage->engine_path);
    } else if (stage->input_h == 1080) {
        snprintf(w->trt_engine_path_1080, sizeof(w->trt_engine_path_1080), "%s", stage->engine_path);
    }
}

static int trt_stage_bind(BakeWorker* w, const DProcModelStage* stage) {
    if (!w || !stage) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_bind_null");
        return -1;
    }
    const int mode = trt_stage_family_to_graph_model_family_code(stage->family_code);
    if (mode < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_bad_family id=%s family=%s",
                 stage->id, dproc_model_family_name(stage->family_code));
        return -1;
    }
    const int saved_mode = w->graph_model_family_code;
    const int saved_bin = w->graph_model_input_h;
    trt_stage_copy_path(w, stage);
    w->graph_model_family_code = mode;
    w->graph_model_input_h = stage->input_h;
    int rc = trt_upscaler_select_engine(w, stage->input_w, stage->input_h);
    w->graph_model_family_code = saved_mode;
    w->graph_model_input_h = saved_bin;
    return rc;
}

static void trt_stage_rotate_slot(BakeWorker* w) {
    w->trt_slot ^= 1;
    w->trt_active_out_chw = (w->trt_slot == 0)
        ? w->trt_active_out_chw_a
        : w->trt_active_out_chw_b;
}

int trt_stage_run_rgba_to_chw(BakeWorker* w, DProcModelStage* stage,
                              CUdeviceptr source_rgba, int src_w, int src_h,
                              CUdeviceptr* out_chw, int* out_w, int* out_h) {
    if (trt_stage_bind(w, stage) < 0) return -1;
    if (!w->trt_active || !source_rgba || !out_chw || !out_w || !out_h) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_bad_rgba_input id=%s", stage ? stage->id : "");
        return -1;
    }
    trt_stage_rotate_slot(w);
    double t0 = now_seconds();
    launch_rgba8_to_rgb_chw_resampled(
        (void*)(uintptr_t)source_rgba,
        (void*)(uintptr_t)w->trt_active_in_chw,
        src_h, src_w,
        w->trt_active_in_h, w->trt_active_in_w,
        (void*)w->cu_stream);
    char err[256] = {0};
    if (trt_sr_engine_run((TrtSrEngine*)w->trt_active,
                        w->trt_active_in_chw,
                        w->trt_active_out_chw, err) < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_run id=%s err=%s", stage->id, err);
        return -1;
    }
    cuEventRecord(w->ev_trt_done, w->cu_stream);
    if (stage) {
        stage->runs++;
        stage->stage_seconds += now_seconds() - t0;
    }
    *out_chw = w->trt_active_out_chw;
    *out_w = w->trt_active_out_w;
    *out_h = w->trt_active_out_h;
    return 0;
}

int trt_stage_run_chw_to_chw(BakeWorker* w, DProcModelStage* stage,
                             CUdeviceptr in_chw, int in_w, int in_h,
                             CUdeviceptr* out_chw, int* out_w, int* out_h) {
    if (trt_stage_bind(w, stage) < 0) return -1;
    if (!w->trt_active || !in_chw || !out_chw || !out_w || !out_h) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_bad_chw_input id=%s", stage ? stage->id : "");
        return -1;
    }
    trt_stage_rotate_slot(w);
    double t0 = now_seconds();
    launch_rgb_chw_to_rgb_chw_resampled(
        (void*)(uintptr_t)in_chw,
        (void*)(uintptr_t)w->trt_active_in_chw,
        in_h, in_w,
        w->trt_active_in_h, w->trt_active_in_w,
        (void*)w->cu_stream);
    char err[256] = {0};
    if (trt_sr_engine_run((TrtSrEngine*)w->trt_active,
                        w->trt_active_in_chw,
                        w->trt_active_out_chw, err) < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_stage_run id=%s err=%s", stage->id, err);
        return -1;
    }
    cuEventRecord(w->ev_trt_done, w->cu_stream);
    if (stage) {
        stage->runs++;
        stage->stage_seconds += now_seconds() - t0;
    }
    *out_chw = w->trt_active_out_chw;
    *out_w = w->trt_active_out_w;
    *out_h = w->trt_active_out_h;
    return 0;
}

static void destroy_trt_upscaler_engines(BakeWorker* w) {
    if (!w) return;
    if (w->trt_engine_480)  { trt_sr_engine_destroy((TrtSrEngine*)w->trt_engine_480);  w->trt_engine_480  = NULL; }
    if (w->trt_engine_720)  { trt_sr_engine_destroy((TrtSrEngine*)w->trt_engine_720);  w->trt_engine_720  = NULL; }
    if (w->trt_engine_1080) { trt_sr_engine_destroy((TrtSrEngine*)w->trt_engine_1080); w->trt_engine_1080 = NULL; }
    if (w->trt_in_chw_480)     { cuMemFree(w->trt_in_chw_480);     w->trt_in_chw_480     = 0; }
    if (w->trt_out_chw_480_a)  { cuMemFree(w->trt_out_chw_480_a);  w->trt_out_chw_480_a  = 0; }
    if (w->trt_out_chw_480_b)  { cuMemFree(w->trt_out_chw_480_b);  w->trt_out_chw_480_b  = 0; }
    if (w->trt_in_chw_720)     { cuMemFree(w->trt_in_chw_720);     w->trt_in_chw_720     = 0; }
    if (w->trt_out_chw_720_a)  { cuMemFree(w->trt_out_chw_720_a);  w->trt_out_chw_720_a  = 0; }
    if (w->trt_out_chw_720_b)  { cuMemFree(w->trt_out_chw_720_b);  w->trt_out_chw_720_b  = 0; }
    if (w->trt_in_chw_1080)    { cuMemFree(w->trt_in_chw_1080);    w->trt_in_chw_1080    = 0; }
    if (w->trt_out_chw_1080_a) { cuMemFree(w->trt_out_chw_1080_a); w->trt_out_chw_1080_a = 0; }
    if (w->trt_out_chw_1080_b) { cuMemFree(w->trt_out_chw_1080_b); w->trt_out_chw_1080_b = 0; }
    w->trt_active = NULL;
    w->trt_active_in_chw = 0;
    w->trt_active_out_chw = 0;
    w->trt_active_in_w = w->trt_active_in_h = 0;
    w->trt_active_out_w = w->trt_active_out_h = 0;
    w->trt_family_480 = 0;
    w->trt_family_720 = 0;
    w->trt_family_1080 = 0;
}

int trt_upscaler_run_source_rgba_to(BakeWorker* w, CUdeviceptr source_rgba, CUdeviceptr out_rgba_4k) {
    if (!w || !w->trt_active) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_upscaler_no_active_engine");
        return -1;
    }
    if (!source_rgba || !out_rgba_4k || w->vsr_input_width <= 0 || w->vsr_input_height <= 0) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_upscaler_bad_source_rgba");
        return -1;
    }

    w->trt_slot ^= 1;
    w->trt_active_out_chw = (w->trt_slot == 0)
        ? w->trt_active_out_chw_a
        : w->trt_active_out_chw_b;

    launch_rgba8_to_rgb_chw_resampled(
        (void*)(uintptr_t)source_rgba,
        (void*)(uintptr_t)w->trt_active_in_chw,
        w->vsr_input_height, w->vsr_input_width,
        w->trt_active_in_h, w->trt_active_in_w,
        (void*)w->cu_stream);

    char err[256] = {0};
    if (trt_sr_engine_run((TrtSrEngine*)w->trt_active,
                        w->trt_active_in_chw,
                        w->trt_active_out_chw, err) < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "trt_upscaler_run=%s", err);
        return -1;
    }

    cuEventRecord(w->ev_trt_done, w->cu_stream);
    cuStreamWaitEvent(w->cu_stream_b, w->ev_trt_done, 0);
    launch_rgb_chw_to_rgba8_resampled(
        (void*)(uintptr_t)w->trt_active_out_chw,
        (void*)(uintptr_t)out_rgba_4k,
        w->trt_active_out_h, w->trt_active_out_w,
        BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
        (void*)w->cu_stream_b);
    return 0;
}
