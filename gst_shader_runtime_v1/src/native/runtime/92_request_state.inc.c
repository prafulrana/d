
// ----------------------------------------------------------------------------
// bake_run
// ----------------------------------------------------------------------------

// Prepared-context cache. A worker that gets {prep_only:true} as its first
// JSON command runs bake_prepare to populate this slot — once a viewer request
// arrives with start_seconds we move the prepared ctx into bake_run_prepared
// without paying the avformat_open / find_stream_info cost again.
static BakeCtx g_prepared_ctx = {0};
static int    g_prepared_valid = 0;

// Sets graph-resolved runtime knobs on the worker from the request.
// MUST be called AFTER ensure_worker_ready / bake_worker_init since those
// memset the worker struct and would clobber these fields.
static void apply_request_runtime(BakeWorker* w, const BakeRequest* req) {
    if (!w || !req) return;
    w->graph_model_family_code = DPROC_MODEL_FAMILY_NONE;
    w->graph_model_input_h = 0;
    w->model_stage_count = req->model_stage_count > DPROC_MAX_MODEL_STAGES
        ? DPROC_MAX_MODEL_STAGES
        : req->model_stage_count;
    for (int i = 0; i < w->model_stage_count; i++) {
        w->model_stages[i] = req->model_stages[i];
        if (w->graph_model_family_code == DPROC_MODEL_FAMILY_NONE &&
            w->model_stages[i].kind_code == DPROC_MODEL_KIND_MODEL) {
            w->graph_model_family_code = w->model_stages[i].family_code;
            w->graph_model_input_h = w->model_stages[i].input_h;
        }
    }
    if (req->d_pipeline_json && req->d_pipeline_json[0]) {
        snprintf(w->d_pipeline_json, sizeof(w->d_pipeline_json), "%s", req->d_pipeline_json);
    } else {
        snprintf(w->d_pipeline_json, sizeof(w->d_pipeline_json), "%s", "{}");
    }
    // Deband + temporal denoise: 0 = off. Clamp to safe ranges so an OOB
    // request can't crank the kernels into nonsense regimes.
    w->deband_strength = (req->deband_strength > 0.0f && req->deband_strength <= 2.0f) ? req->deband_strength : 0.0f;
    w->temporal_denoise_strength = (req->temporal_denoise_strength > 0.0f && req->temporal_denoise_strength <= 1.0f) ? req->temporal_denoise_strength : 0.0f;
    w->temporal_denoise_luma_max = (req->temporal_denoise_luma_max > 0.0f && req->temporal_denoise_luma_max <= 1.0f) ? req->temporal_denoise_luma_max : 0.0f;
    // Copy per-bin engine paths off the request buffer. The request strings
    // point into the parser's `line` buffer which gets free'd; we need our
    // own storage. Empty path = fall through to compiled-in default.
    if (req->trt_engine_path_480 && req->trt_engine_path_480[0]) {
        snprintf(w->trt_engine_path_480, sizeof(w->trt_engine_path_480), "%s", req->trt_engine_path_480);
    }
    if (req->trt_engine_path_720 && req->trt_engine_path_720[0]) {
        snprintf(w->trt_engine_path_720, sizeof(w->trt_engine_path_720), "%s", req->trt_engine_path_720);
    }
    if (req->trt_engine_path_1080 && req->trt_engine_path_1080[0]) {
        snprintf(w->trt_engine_path_1080, sizeof(w->trt_engine_path_1080), "%s", req->trt_engine_path_1080);
    }
    // runtime_state contract: worker will write a JSON snapshot here AFTER
    // engine load so the panel can confirm what loaded vs what was picked.
    if (req->runtime_state_path && req->runtime_state_path[0]) {
        snprintf(w->runtime_state_path, sizeof(w->runtime_state_path), "%s", req->runtime_state_path);
    }
    w->audio_passthrough_active = req->audio_passthrough ? 1 : 0;
    w->audio_maxine_active = 0;
    w->live_clock_mode = req->live_clock_mode ? 1 : 0;
    w->audio_pacing_mode = req->audio_pacing_mode == DPROC_AUDIO_PACING_VIDEO_GATED
        ? DPROC_AUDIO_PACING_VIDEO_GATED
        : DPROC_AUDIO_PACING_SOURCE_PTS;
    w->max_audio_lead_ms = req->max_audio_lead_ms;
    w->max_av_delta_ms = req->max_av_delta_ms;
    fprintf(stderr, "[d_native_processor] runtime: graph_model_family_code=%d upscaler_family=%s model_input_h=%d model_stages=%d audio_passthrough=%d audio_superres_mode=%s live_clock_mode=%d audio_pacing_mode=%d max_audio_lead_ms=%d max_av_delta_ms=%d deband=%.3f temporal_denoise=%.3f@%.2f engines=[480:%s|720:%s|1080:%s]\n",
            w->graph_model_family_code, dproc_model_family_name(w->graph_model_family_code), w->graph_model_input_h,
            w->model_stage_count,
            w->audio_passthrough_active,
            audio_superres_mode_name(req->audio_superres_mode),
            w->live_clock_mode,
            w->audio_pacing_mode,
            w->max_audio_lead_ms,
            w->max_av_delta_ms,
            w->deband_strength, w->temporal_denoise_strength, w->temporal_denoise_luma_max,
            trt_engine_path(w, 480), trt_engine_path(w, 720), trt_engine_path(w, 1080));
    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        fprintf(stderr,
                "[d_native_processor] d_model_stage parsed index=%d id=%s kind=%s engine=%s family=%s pass_through=%d input=%dx%d@%d output=%dx%d@%d infer_fps=%d motion_strength=%.3f motion_guide=%s cadence_owner=%s pts_policy=%s frame_hold_policy=%s path=%s\n",
                i, s->id, s->kind, dproc_model_engine_name(s->engine_code),
                s->family[0] ? s->family : dproc_model_family_name(s->family_code),
                s->pass_through,
                s->input_w, s->input_h, s->input_fps,
                s->output_w, s->output_h, s->output_fps,
                s->infer_fps, s->motion_strength, s->motion_guide, s->timing_role, s->pts_policy,
                s->frame_hold_policy, s->engine_path);
    }
}

// Write a JSON snapshot of what the worker actually loaded, after engine
// + audio init have completed. Called once per spawn — the panel reads
// this and compares against its pick. Mismatch → red banner. Match → ok.
static void ensure_parent_dir(const char* path) {
    if (!path || !path[0]) return;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char* slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            fprintf(stderr, "[d_native_processor] runtime_state mkdir failed path=%s errno=%d\n", tmp, errno);
            *p = '/';
            return;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "[d_native_processor] runtime_state mkdir failed path=%s errno=%d\n", tmp, errno);
    }
}

static void bake_write_runtime_state(BakeWorker* w, int src_w, int src_h, int active_bin) {
    if (!w || !w->runtime_state_path[0]) return;
    if (active_bin == 0) active_bin = w->graph_model_input_h;
    ensure_parent_dir(w->runtime_state_path);
    FILE* fp = fopen(w->runtime_state_path, "w");
    if (!fp) {
        fprintf(stderr, "[d_native_processor] runtime_state write failed path=%s errno=%d\n", w->runtime_state_path, errno);
        return;
    }
    const DProcModelStage* first_model = NULL;
    const DProcModelStage* last_model = NULL;
    for (int i = 0; i < w->model_stage_count; i++) {
        if (w->model_stages[i].kind_code != DPROC_MODEL_KIND_MODEL) continue;
        if (w->model_stages[i].pass_through) continue;
        if (!first_model) first_model = &w->model_stages[i];
        last_model = &w->model_stages[i];
    }
    const char* upscaler_family = first_model
        ? dproc_model_family_name(first_model->family_code)
        : dproc_model_family_name(w->graph_model_family_code);
    const int using_maxine = first_model
        ? first_model->family_code == DPROC_MODEL_FAMILY_MAXINE
        : (w->graph_model_family_code == DPROC_MODEL_FAMILY_MAXINE);
    const char* engine_path = using_maxine ? "NvVFX VideoSuperRes" : trt_engine_path(w, active_bin);
    const int engine_in_w = first_model ? first_model->input_w : (using_maxine ? src_w : w->trt_active_in_w);
    const int engine_in_h = first_model ? first_model->input_h : (using_maxine ? src_h : w->trt_active_in_h);
    const int engine_out_w = last_model ? last_model->output_w : (using_maxine ? w->vsr_output_width : w->trt_active_out_w);
    const int engine_out_h = last_model ? last_model->output_h : (using_maxine ? w->vsr_output_height : w->trt_active_out_h);
    const char* normalize_mode = "direct-4k";
    if (engine_out_w < BAKE_OUTPUT_WIDTH || engine_out_h < BAKE_OUTPUT_HEIGHT) {
        normalize_mode = "near4k-upfill";
    } else if (engine_out_w > BAKE_OUTPUT_WIDTH || engine_out_h > BAKE_OUTPUT_HEIGHT) {
        normalize_mode = "supersample-downscale";
    }
    const char* graph_json = w->d_pipeline_json[0] ? w->d_pipeline_json : "{}";
    fprintf(fp,
        "{\"upscaler_family\":\"%s\",\"upscaler_bin\":%d,\"engine_path\":\"%s\","
        "\"engine_in_w\":%d,\"engine_in_h\":%d,\"engine_out_w\":%d,\"engine_out_h\":%d,"
        "\"final_out_w\":%d,\"final_out_h\":%d,\"normalize_mode\":\"%s\","
        "\"src_w\":%d,\"src_h\":%d,\"audio_passthrough\":%d,\"audio_maxine_active\":%d,"
        "\"live_clock_mode\":%d,\"audio_pacing_mode\":%d,\"max_audio_lead_ms\":%d,\"max_av_delta_ms\":%d,"
        "\"dPipeline\":%s,\"model_stage_count\":%d,\"modelStages\":[",
        upscaler_family, active_bin, engine_path,
        engine_in_w, engine_in_h, engine_out_w, engine_out_h,
        BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT, normalize_mode,
        src_w, src_h, w->audio_passthrough_active, w->audio_maxine_active,
        w->live_clock_mode, w->audio_pacing_mode, w->max_audio_lead_ms, w->max_av_delta_ms,
        graph_json, w->model_stage_count);
    for (int i = 0; i < w->model_stage_count; i++) {
        DProcModelStage* s = &w->model_stages[i];
        fprintf(fp,
            "%s{\"id\":\"%s\",\"label\":\"%s\",\"kind\":\"%s\",\"engine\":\"%s\","
            "\"family\":\"%s\",\"passThrough\":%d,\"engine_path\":\"%s\",\"input\":{\"width\":%d,\"height\":%d,\"fps\":%d},"
            "\"output\":{\"width\":%d,\"height\":%d,\"fps\":%d},\"infer_fps\":%d,"
            "\"params\":{\"motionStrength\":%.3f,\"motionGuide\":\"%s\"},"
            "\"timing\":{\"inputFps\":%d,\"inferFps\":%d,\"outputFps\":%d,"
            "\"cadenceOwner\":\"%s\",\"ptsPolicy\":\"%s\",\"frameHoldPolicy\":\"%s\"},"
            "\"ready\":%d,\"runs\":%lld,\"cadence_dropped\":%lld,\"over_budget\":%lld,\"stage_seconds\":%.6f}",
            i ? "," : "",
            s->id, s->label, s->kind, dproc_model_engine_name(s->engine_code),
            s->family[0] ? s->family : dproc_model_family_name(s->family_code),
            s->pass_through, s->engine_path,
            s->input_w, s->input_h, s->input_fps,
            s->output_w, s->output_h, s->output_fps,
            s->infer_fps,
            s->motion_strength, s->motion_guide,
            s->input_fps, s->infer_fps, s->output_fps,
            s->timing_role, s->pts_policy, s->frame_hold_policy,
            s->ready, s->runs, s->cadence_dropped,
            s->over_budget, s->stage_seconds);
    }
    fprintf(fp, "]}\n");
    fclose(fp);
    fprintf(stderr, "[d_native_processor] runtime_state written family=%s bin=%d engine_in=%dx%d engine_out=%dx%d final_out=%dx%d normalize=%s src=%dx%d audio_pt=%d audio_maxine=%d live_clock_mode=%d audio_pacing_mode=%d max_audio_lead_ms=%d\n",
        upscaler_family, active_bin, engine_in_w, engine_in_h,
        engine_out_w, engine_out_h, BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT, normalize_mode,
        src_w, src_h, w->audio_passthrough_active, w->audio_maxine_active,
        w->live_clock_mode, w->audio_pacing_mode, w->max_audio_lead_ms);
}

int bake_prepare(BakeWorker* w, const BakeRequest* req, BakeResult* res) {
    memset(res, 0, sizeof(*res));
    g_last_error[0] = 0;
    if (g_prepared_valid) { close_ctx(&g_prepared_ctx); memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx)); g_prepared_valid = 0; }
    if (av_hwdevice_ctx_create(&g_prepared_ctx.hw_device_ref, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0) < 0) {
        snprintf(res->error, sizeof(res->error), "hwdev_create");
        return -1;
    }
    if (open_input(&g_prepared_ctx, req) < 0) {
        snprintf(res->error, sizeof(res->error), "%s", g_last_error[0] ? g_last_error : "open_input");
        close_ctx(&g_prepared_ctx);
        memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx));
        return -1;
    }
    if (ensure_worker_ready(w) < 0) {
        snprintf(res->error, sizeof(res->error), "%s", g_last_error[0] ? g_last_error : "worker_init");
        close_ctx(&g_prepared_ctx);
        memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx));
        return -1;
    }
    apply_request_runtime(w, req);
    if (configure_vsr_for_ctx(w, &g_prepared_ctx) < 0) {
        snprintf(res->error, sizeof(res->error), "%s", g_last_error[0] ? g_last_error : "vsr_configure");
        close_ctx(&g_prepared_ctx);
        memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx));
        return -1;
    }
    warm_seek_index(&g_prepared_ctx);
    g_prepared_valid = 1;
    res->ok = 1;
    return 0;
}

void bake_release_prepared(BakeWorker* w) {
    (void)w;
    if (!g_prepared_valid) return;
    close_ctx(&g_prepared_ctx);
    memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx));
    g_prepared_valid = 0;
}

static int parse_pipeline_manifests(BakeCtx* c, const BakeRequest* req) {
    if (dproc_pipeline_manifest_parse(c->stages, &c->stage_count, req->pipeline_manifest_json,
                                     g_last_error, sizeof(g_last_error)) < 0) {
        return -1;
    }
    char audio_err[256] = {0};
    if (dproc_pipeline_manifest_parse(c->audio_stages, &c->audio_stage_count, req->audio_pipeline_manifest_json,
                                     audio_err, sizeof(audio_err)) < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "audio_%s", audio_err[0] ? audio_err : "pipeline_manifest");
        return -1;
    }
    fprintf(stderr, "[d_native_processor] pipeline manifests parsed video_stages=%d audio_stages=%d\n",
            c->stage_count, c->audio_stage_count);
    const uint32_t required[] = {
        STAGE_AUDIO_DECODE,
        STAGE_AUDIO_TO_16K_MONO,
        STAGE_MAXINE_AUDIO_CLEANUP,
        STAGE_MAXINE_AUDIO_SUPERRES,
        STAGE_AUDIO_EQ_PROFILE,
        STAGE_AUDIO_DELAY_SYNC,
        STAGE_AUDIO_TO_STEREO_48K,
        STAGE_AUDIO_AAC_TRANSPORT,
    };
    int last = -1;
    for (int r = 0; r < (int)(sizeof(required) / sizeof(required[0])); r++) {
        int found = -1;
        for (int i = 0; i < c->audio_stage_count; i++) {
            if (c->audio_stages[i].id_hash == required[r]) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "audio_pipeline_missing_required_stage=%u", required[r]);
            return -1;
        }
        if (found <= last) {
            snprintf(g_last_error, sizeof(g_last_error), "audio_pipeline_order_invalid");
            return -1;
        }
        last = found;
    }
    return 0;
}
