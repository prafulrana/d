int run_source_prep_stage(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                          AVFrame* in_frame, int input_format) {
    if (!pipeline_stage_enabled(c, STAGE_NV12_TO_RGB_CHW)) {
        snprintf(g_last_error, sizeof(g_last_error), "pipeline_missing_source_prep");
        return -1;
    }
    if (!w->pre_vsr_rgba || w->vsr_input_width <= 0 || w->vsr_input_height <= 0) {
        snprintf(g_last_error, sizeof(g_last_error), "source_rgba_not_configured");
        return -1;
    }
    double tp = stage_now_seconds();
    launch_nv12_pre_vsr_filter(
        in_frame->data[0], in_frame->data[1],
        (void*)(uintptr_t)w->pre_vsr_rgba,
        in_frame->height, in_frame->width,
        in_frame->linesize[0], in_frame->linesize[1],
        input_format,
        w->vsr_input_height, w->vsr_input_width,
        req->contrast, req->saturation, 1.0f / req->gamma,
        req->cas_strength, (void*)w->cu_stream
    );
    c->stage_pre_s += stage_now_seconds() - tp;
    return 0;
}

static int run_maxine_upscaler_source_rgba(BakeWorker* w, CUdeviceptr source_rgba,
                                           CUdeviceptr out_rgba_4k) {
    if (!w->vsr_effect) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_vsr_not_loaded");
        return -1;
    }
    const int out_w = w->vsr_output_width > 0 ? w->vsr_output_width : BAKE_OUTPUT_WIDTH;
    const int out_h = w->vsr_output_height > 0 ? w->vsr_output_height : BAKE_OUTPUT_HEIGHT;
    if (out_w != BAKE_OUTPUT_WIDTH || out_h != BAKE_OUTPUT_HEIGHT) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "maxine_non_canon_output_forbidden=%dx%d canon=%dx%d",
                 out_w, out_h, BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT);
        return -1;
    }
    CUdeviceptr raw_out = out_rgba_4k;
    if (!raw_out) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_raw_buffer_missing");
        return -1;
    }

    const int in_w = w->maxine_input_width > 0 ? w->maxine_input_width : w->vsr_input_width;
    const int in_h = w->maxine_input_height > 0 ? w->maxine_input_height : w->vsr_input_height;
    if (wrap_rgba_nvcv(&w->vsr_src_im, (void*)(uintptr_t)source_rgba,
                       in_w, in_h) < 0) {
        return -1;
    }
    if (wrap_rgba_nvcv(&w->vsr_dst_im, (void*)(uintptr_t)raw_out,
                       out_w, out_h) < 0) {
        return -1;
    }
    STAGE_CHECK_NV(NvVFX_SetImage(w->vsr_effect, NVVFX_INPUT_IMAGE, &w->vsr_src_im), "setInImg");
    STAGE_CHECK_NV(NvVFX_SetImage(w->vsr_effect, NVVFX_OUTPUT_IMAGE, &w->vsr_dst_im), "setOutImg");
    {
        NvCV_Status vsr_status = NvVFX_Run(w->vsr_effect, 1);
        if (vsr_status != NVCV_SUCCESS) {
            snprintf(g_last_error, sizeof(g_last_error), "vsr_run=%d", vsr_status);
            return -1;
        }
    }
    cuEventRecord(w->ev_vsr_done, w->cu_stream);
    cuStreamWaitEvent(w->cu_stream_b, w->ev_vsr_done, 0);
    return 0;
bail:
    return -1;
}

static void note_d_model_stage_budget(DProcModelStage* stage, double elapsed_s) {
    if (!stage || stage->infer_fps <= 0 || elapsed_s <= 0.0) return;
    const double budget_s = 1.0 / (double)stage->infer_fps;
    if (elapsed_s <= budget_s) return;
    stage->over_budget++;
    if (stage->over_budget <= 8 || (stage->over_budget % 120) == 0) {
        fprintf(stderr,
                "[d_native_processor] stage_over_budget id=%s family=%s elapsed_ms=%.3f budget_ms=%.3f count=%lld\n",
                stage->id,
                dproc_model_family_name(stage->family_code),
                elapsed_s * 1000.0,
                budget_s * 1000.0,
                stage->over_budget);
    }
}

typedef struct {
    CUdeviceptr rgba;
    int rgba_w;
    int rgba_h;
    CUdeviceptr chw;
    int chw_w;
    int chw_h;
} DProcSurface;

static int d_sync_stream_b_to_main(BakeWorker* w, const char* label) {
    CUresult cs = cuEventRecord(w->ev_vsr_done, w->cu_stream_b);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "%s_event_record=%d", label, cs);
        return -1;
    }
    cs = cuStreamWaitEvent(w->cu_stream, w->ev_vsr_done, 0);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "%s_event_wait=%d", label, cs);
        return -1;
    }
    return 0;
}

static int d_surface_to_rgba(BakeWorker* w, DProcSurface* s,
                             CUdeviceptr out_rgba, int out_w, int out_h) {
    if (s->rgba) {
        if (s->rgba != out_rgba || s->rgba_w != out_w || s->rgba_h != out_h) {
            launch_rgba8_to_rgba8_resampled((void*)(uintptr_t)s->rgba,
                                            (void*)(uintptr_t)out_rgba,
                                            s->rgba_h, s->rgba_w,
                                            out_h, out_w,
                                            (void*)w->cu_stream_b);
            if (d_sync_stream_b_to_main(w, "rgba_surface_sync") < 0) return -1;
        }
        s->rgba = out_rgba; s->rgba_w = out_w; s->rgba_h = out_h;
        s->chw = 0; s->chw_w = s->chw_h = 0;
        return 0;
    }
    if (!s->chw) {
        snprintf(g_last_error, sizeof(g_last_error), "d_pipeline_surface_empty");
        return -1;
    }
    cuStreamWaitEvent(w->cu_stream_b, w->ev_trt_done, 0);
    launch_rgb_chw_to_rgba8_resampled((void*)(uintptr_t)s->chw,
                                      (void*)(uintptr_t)out_rgba,
                                      s->chw_h, s->chw_w,
                                      out_h, out_w,
                                      (void*)w->cu_stream_b);
    if (d_sync_stream_b_to_main(w, "trt_chw_surface_sync") < 0) return -1;
    s->rgba = out_rgba; s->rgba_w = out_w; s->rgba_h = out_h;
    s->chw = 0; s->chw_w = s->chw_h = 0;
    return 0;
}

static int run_d_model_one(BakeCtx* c, BakeWorker* w, DProcModelStage* stage,
                           DProcSurface* s) {
    (void)c;
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL &&
        stage->engine_code == DPROC_MODEL_ENGINE_TRT &&
        stage->pass_through) {
        stage->runs++;
        return 0;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL &&
        stage->engine_code == DPROC_MODEL_ENGINE_TRT) {
        double before = stage->stage_seconds;
        int rc = s->chw
            ? trt_stage_run_chw_to_chw(w, stage, s->chw, s->chw_w, s->chw_h,
                                       &s->chw, &s->chw_w, &s->chw_h)
            : trt_stage_run_rgba_to_chw(w, stage, s->rgba, s->rgba_w, s->rgba_h,
                                        &s->chw, &s->chw_w, &s->chw_h);
        if (rc < 0) return -1;
        s->rgba = 0; s->rgba_w = s->rgba_h = 0;
        note_d_model_stage_budget(stage, stage->stage_seconds - before);
        return 0;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MOTION) {
        stage->runs++;
        return 0;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL &&
        stage->engine_code == DPROC_MODEL_ENGINE_MAXINE) {
        if (d_surface_to_rgba(w, s, w->d_pipeline_rgba_a,
                              stage->input_w, stage->input_h) < 0) return -1;
        double t0 = stage_now_seconds();
        if (run_maxine_upscaler_source_rgba(w, s->rgba, w->vsr_out_rgba) < 0) return -1;
        double elapsed = stage_now_seconds() - t0;
        stage->runs++;
        stage->stage_seconds += elapsed;
        note_d_model_stage_budget(stage, elapsed);
        s->rgba = w->vsr_out_rgba;
        s->rgba_w = BAKE_OUTPUT_WIDTH;
        s->rgba_h = BAKE_OUTPUT_HEIGHT;
        return 0;
    }
    return 0;
}

int run_d_model_pipeline_range_to_rgba(BakeCtx* c, BakeWorker* w,
                                       CUdeviceptr source_rgba,
                                       int source_w, int source_h,
                                       int start_index, int end_index,
                                       CUdeviceptr out_rgba,
                                       int out_w, int out_h) {
    if (!w || w->model_stage_count <= 0) return 0;
    DProcSurface s = { source_rgba, source_w, source_h, 0, 0, 0 };
    if (start_index < 0) start_index = 0;
    if (end_index < 0 || end_index > w->model_stage_count) end_index = w->model_stage_count;
    for (int i = start_index; i < end_index; i++) {
        if (run_d_model_one(c, w, &w->model_stages[i], &s) < 0) return -1;
    }
    if (d_surface_to_rgba(w, &s, out_rgba, out_w, out_h) < 0) return -1;
    return 0;
}

static int run_d_model_pipeline_stage(BakeCtx* c, BakeWorker* w, CUdeviceptr source_rgba) {
    return run_d_model_pipeline_range_to_rgba(c, w, source_rgba,
                                              w->vsr_input_width, w->vsr_input_height,
                                              0, w->model_stage_count,
                                              w->vsr_out_rgba,
                                              BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT);
}

static int run_upscaler_stage(BakeCtx* c, BakeWorker* w, CUdeviceptr source_rgba) {
    if (!pipeline_stage_enabled(c, STAGE_UPSCALER)) {
        snprintf(g_last_error, sizeof(g_last_error), "pipeline_missing_upscaler");
        return -1;
    }
    if (source_rgba == w->interp_pre_vsr_rgba) {
        cuEventRecord(w->ev_vsr_done, w->cu_stream_b);
        cuStreamWaitEvent(w->cu_stream, w->ev_vsr_done, 0);
    }
    double tv = stage_now_seconds();
    if (w->model_stage_count > 0) {
        if (run_d_model_pipeline_stage(c, w, source_rgba) < 0) return -1;
    } else {
        snprintf(g_last_error, sizeof(g_last_error), "d_pipeline_model_stages_missing");
        return -1;
    }
    c->stage_vsr_s += stage_now_seconds() - tv;
    return 0;
}

int run_upscaler_stage_from_model_index(BakeCtx* c, BakeWorker* w,
                                        CUdeviceptr source_rgba,
                                        int source_w, int source_h,
                                        int start_index) {
    if (!pipeline_stage_enabled(c, STAGE_UPSCALER)) {
        snprintf(g_last_error, sizeof(g_last_error), "pipeline_missing_upscaler");
        return -1;
    }
    double tv = stage_now_seconds();
    if (run_d_model_pipeline_range_to_rgba(c, w, source_rgba,
                                           source_w, source_h,
                                           start_index, w->model_stage_count,
                                           w->vsr_out_rgba,
                                           BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT) < 0) {
        return -1;
    }
    c->stage_vsr_s += stage_now_seconds() - tv;
    return 0;
}

static int ensure_nvof_ready_for_dims(BakeWorker* w, int target_w, int target_h) {
    if (target_w <= 0 || target_h <= 0) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_bad_stage_dims=%dx%d", target_w, target_h);
        return -1;
    }
    if (w->nvof_ready && w->nvof_target_w == target_w && w->nvof_target_h == target_h) return 0;
    nvof_destroy(w);
    if (nvof_init(w, target_w, target_h) < 0) return -1;
    w->nvof_ready = 1;
    return 0;
}

int run_source_nvof_stage(BakeCtx* c, BakeWorker* w) {
    double tn = stage_now_seconds();
    if (ensure_nvof_ready_for_dims(w, w->vsr_input_width, w->vsr_input_height) < 0) return -1;
    if (nvof_execute(w, w->prev_pre_vsr_rgba, w->pre_vsr_rgba) < 0) return -1;
    c->stage_nvof_s += stage_now_seconds() - tn;
    return 0;
}

int run_output_nvof_stage(BakeCtx* c, BakeWorker* w) {
    double tn = stage_now_seconds();
    if (ensure_nvof_ready_for_dims(w, BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT) < 0) return -1;
    CUresult cs = cuEventRecord(w->ev_vsr_done, w->cu_stream_b);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "output_nvof_event_record=%d", cs);
        return -1;
    }
    cs = cuStreamWaitEvent(w->cu_stream, w->ev_vsr_done, 0);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "output_nvof_event_wait=%d", cs);
        return -1;
    }
    if (nvof_execute(w, w->prev_final_rgba, w->final_rgba) < 0) return -1;
    c->stage_nvof_s += stage_now_seconds() - tn;
    return 0;
}
