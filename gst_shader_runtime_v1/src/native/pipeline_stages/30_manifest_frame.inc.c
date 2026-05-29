int run_manifest_encode_rgba_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                                   CUdeviceptr rgba, int64_t pts,
                                   int* n_out, long long* bytes_out) {
    double te = stage_now_seconds();
    if (encode_final_rgba_frame(c, w, req, (void*)(uintptr_t)rgba,
                                pts, n_out, bytes_out) < 0) {
        return -1;
    }
    c->stage_encode_s += stage_now_seconds() - te;
    return 0;
}

static int run_manifest_frame_impl(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                                   CUdeviceptr source_rgba, int64_t pts,
                                   int source_w, int source_h,
                                   int model_start_index,
                                   int frame_seed, int have_prev_final,
                                   int* flow_ready_io,
                                   int encode,
                                   int* n_out, long long* bytes_out) {
    int upscaled = 0;
    int finalized = 0;
    int flow_ready = flow_ready_io ? *flow_ready_io : 0;
    for (int i = 0; i < c->stage_count; i++) {
        switch (c->stages[i].id_hash) {
            case STAGE_UPSCALER:
                if (model_start_index > 0) {
                    if (run_upscaler_stage_from_model_index(c, w, source_rgba,
                                                            source_w, source_h,
                                                            model_start_index) < 0) return -1;
                } else if (run_upscaler_stage(c, w, source_rgba) < 0) return -1;
                upscaled = 1;
                break;
            case STAGE_RGB_CHW_TO_RGBA8:
                if (!upscaled) {
                    snprintf(g_last_error, sizeof(g_last_error), "pipeline_rgb_chw_before_upscaler");
                    return -1;
                }
                break;
            case STAGE_POST_VSR_FINALIZE:
                if (!upscaled) {
                    snprintf(g_last_error, sizeof(g_last_error), "pipeline_post_before_upscaler");
                    return -1;
                }
                {
                    double tp = stage_now_seconds();
                    if (finalize_vsr_frame(w, req, frame_seed) < 0) return -1;
                    c->stage_post_s += stage_now_seconds() - tp;
                }
                finalized = 1;
                break;
            case STAGE_DEBAND_4K:
                if (!finalized) {
                    snprintf(g_last_error, sizeof(g_last_error), "pipeline_deband_before_finalize");
                    return -1;
                }
                {
                    double tp = stage_now_seconds();
                    if (deband_final_frame(w, frame_seed) < 0) return -1;
                    c->stage_post_s += stage_now_seconds() - tp;
                }
                break;
            case STAGE_CUSTOM_SHADER:
                if (!finalized) {
                    snprintf(g_last_error, sizeof(g_last_error), "pipeline_custom_shader_before_finalize");
                    return -1;
                }
                {
                    double tp = stage_now_seconds();
                    if (custom_shader_final_frame(w, req, frame_seed) < 0) return -1;
                    c->stage_post_s += stage_now_seconds() - tp;
                }
                break;
            case STAGE_DLSAA_TEMPORAL:
                if (finalized && have_prev_final && flow_ready &&
                    (req->temporal_strength > 0.001f || req->edge_stability > 0.001f)) {
                    double tt = stage_now_seconds();
                    if (reconstruct_temporal_frame(w, req) < 0) return -1;
                    c->stage_temporal_s += stage_now_seconds() - tt;
                }
                break;
            case STAGE_TEMPORAL_DENOISE:
                if (finalized && have_prev_final && flow_ready && w->temporal_denoise_strength > 0.001f) {
                    double tt = stage_now_seconds();
                    launch_temporal_denoise_rgba(
                        (void*)(uintptr_t)w->final_rgba,
                        (void*)(uintptr_t)w->prev_final_rgba,
                        (void*)(uintptr_t)w->nvof_flow_ptr,
                        (void*)(uintptr_t)w->interp_final_rgba,
                        BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
                        w->nvof_flow_h, w->nvof_flow_w,
                        (int)w->nvof_flow_pitch,
                        w->nvof_grid,
                        w->temporal_denoise_strength,
                        (void*)w->cu_stream_b);
                    STAGE_CHECK_CU(cuMemcpyDtoDAsync(w->final_rgba, w->interp_final_rgba,
                                                     w->vsr_out_bytes, w->cu_stream_b), "temporalDenoiseCopy");
                    c->stage_temporal_s += stage_now_seconds() - tt;
                }
                break;
            case STAGE_NVENC_HEVC:
                if (!finalized) {
                    snprintf(g_last_error, sizeof(g_last_error), "pipeline_encode_before_finalize");
                    return -1;
                }
                if (!encode) return 0;
                if (run_manifest_encode_rgba_frame(c, w, req, w->final_rgba,
                                                   pts, n_out, bytes_out) < 0) return -1;
                return 0;
            default:
                break;
        }
    }
    snprintf(g_last_error, sizeof(g_last_error), "pipeline_missing_nvenc_hevc");
    return -1;
bail:
    return -1;
}

int run_manifest_render_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                              CUdeviceptr source_rgba,
                              int frame_seed, int have_prev_final,
                              int* flow_ready_io) {
    return run_manifest_frame_impl(c, w, req, source_rgba, 0,
                                   w->vsr_input_width, w->vsr_input_height, 0,
                                   frame_seed,
                                   have_prev_final, flow_ready_io, 0, NULL, NULL);
}

int run_manifest_output_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                              CUdeviceptr source_rgba, int64_t pts,
                              int frame_seed, int have_prev_final,
                              int flow_ready,
                              int* n_out, long long* bytes_out) {
    int flow_ready_io = flow_ready;
    return run_manifest_frame_impl(c, w, req, source_rgba, pts,
                                   w->vsr_input_width, w->vsr_input_height, 0,
                                   frame_seed,
                                   have_prev_final, &flow_ready_io, 1, n_out, bytes_out);
}

int run_manifest_output_frame_from_model_index(BakeCtx* c, BakeWorker* w,
                              const BakeRequest* req,
                              CUdeviceptr source_rgba, int source_w, int source_h,
                              int start_index, int64_t pts,
                              int frame_seed, int have_prev_final,
                              int flow_ready,
                              int* n_out, long long* bytes_out) {
    int flow_ready_io = flow_ready;
    return run_manifest_frame_impl(c, w, req, source_rgba, pts,
                                   source_w, source_h, start_index,
                                   frame_seed, have_prev_final,
                                   &flow_ready_io, 1, n_out, bytes_out);
}
