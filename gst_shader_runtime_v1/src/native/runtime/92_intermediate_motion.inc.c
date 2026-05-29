static int d_pipeline_front_half_to_motion_input(BakeCtx* c, BakeWorker* w,
                                                 int motion_idx) {
    if (!w || motion_idx < 0 || motion_idx >= w->model_stage_count) return -1;
    DProcModelStage* motion = &w->model_stages[motion_idx];
    double tv = now_seconds();
    if (run_d_model_pipeline_range_to_rgba(c, w,
                                           w->pre_vsr_rgba,
                                           w->vsr_input_width,
                                           w->vsr_input_height,
                                           0, motion_idx,
                                           w->d_pipeline_rgba_a,
                                           motion->input_w,
                                           motion->input_h) < 0) {
        return -1;
    }
    c->stage_vsr_s += now_seconds() - tv;
    CUresult cs = cuEventRecord(w->ev_vsr_done, w->cu_stream_b);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "intermediate_motion_event_record=%d", cs);
        return -1;
    }
    cs = cuStreamWaitEvent(w->cu_stream, w->ev_vsr_done, 0);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "intermediate_motion_event_wait=%d", cs);
        return -1;
    }
    return 0;
}

static int d_pipeline_seed_intermediate_motion_frame(BakeCtx* c, BakeWorker* w,
                                                     const BakeRequest* req,
                                                     int64_t pts,
                                                     int frame_seed,
                                                     int have_prev_final,
                                                     int* n_out,
                                                     long long* bytes_out) {
    const int motion_idx = d_pipeline_intermediate_motion_index(w);
    const int next_model = d_pipeline_next_scale_model_after(w, motion_idx);
    if (motion_idx < 0 || next_model < 0) return 0;
    DProcModelStage* motion = &w->model_stages[motion_idx];
    if (d_pipeline_front_half_to_motion_input(c, w, motion_idx) < 0) return -1;
    CHECK_CU(cuMemcpyDtoDAsync(w->d_pipeline_rgba_b, w->d_pipeline_rgba_a,
                               (size_t)motion->input_w * motion->input_h * 4,
                               w->cu_stream_b), "seedIntermediateMotionCopy");
    return run_manifest_output_frame_from_model_index(c, w, req,
        w->d_pipeline_rgba_a, motion->input_w, motion->input_h,
        next_model, pts, frame_seed, have_prev_final, 0, n_out, bytes_out);
bail:
    return -1;
}

static int d_pipeline_seed_or_output_first_frame(BakeCtx* c, BakeWorker* w,
                                                 const BakeRequest* req,
                                                 int intermediate_idx,
                                                 int64_t pts, int frame_seed,
                                                 int* n_out,
                                                 long long* bytes_out) {
    if (intermediate_idx >= 0) {
        return d_pipeline_seed_intermediate_motion_frame(c, w, req, pts,
                                                         frame_seed, 0,
                                                         n_out, bytes_out);
    }
    return run_manifest_output_frame(c, w, req, w->pre_vsr_rgba, pts,
                                     frame_seed, 0, 0, n_out, bytes_out);
}

static int d_pipeline_run_intermediate_motion_interval(BakeCtx* c, BakeWorker* w,
                                                       const BakeRequest* req,
                                                       double prev_src_s,
                                                       double curr_src_s,
                                                       double out_fps,
                                                       int n_in,
                                                       int have_prev_final,
                                                       int64_t* next_out_pts,
                                                       int* n_out,
                                                       long long* bytes_out) {
    const int motion_idx = d_pipeline_intermediate_motion_index(w);
    const int next_model = d_pipeline_next_scale_model_after(w, motion_idx);
    if (motion_idx < 0 || next_model < 0) return 0;
    DProcModelStage* motion = &w->model_stages[motion_idx];
    const double interval_s = curr_src_s - prev_src_s;
    if (interval_s <= 0.0) return 1;
    if (d_pipeline_front_half_to_motion_input(c, w, motion_idx) < 0) return -1;
    double tn = now_seconds();
    if (motion->input_w <= 0 || motion->input_h <= 0) {
        snprintf(g_last_error, sizeof(g_last_error), "intermediate_nvof_bad_dims=%dx%d",
                 motion->input_w, motion->input_h);
        return -1;
    }
    if (!w->nvof_ready || w->nvof_target_w != motion->input_w ||
        w->nvof_target_h != motion->input_h) {
        nvof_destroy(w);
        if (nvof_init(w, motion->input_w, motion->input_h) < 0) return -1;
        w->nvof_ready = 1;
    }
    if (nvof_execute(w, w->d_pipeline_rgba_b, w->d_pipeline_rgba_a) < 0) return -1;
    c->stage_nvof_s += now_seconds() - tn;
    while (((double)*next_out_pts / out_fps) <= curr_src_s + 0.000001) {
        const double out_s = (double)*next_out_pts / out_fps;
        double a = (out_s - prev_src_s) / interval_s;
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;
        CUdeviceptr src = w->d_pipeline_rgba_a;
        if (a < 0.999) {
            if (a <= 0.001) {
                src = w->d_pipeline_rgba_b;
            } else {
                launch_nvof_fruc_interpolate(
                    (void*)(uintptr_t)w->d_pipeline_rgba_b,
                    (void*)(uintptr_t)w->d_pipeline_rgba_a,
                    (void*)(uintptr_t)w->nvof_flow_ptr,
                    (void*)(uintptr_t)w->nvof_reverse_flow_ptr,
                    (void*)(uintptr_t)w->interp_final_rgba,
                    motion->input_h, motion->input_w,
                    w->nvof_flow_h, w->nvof_flow_w,
                    (int)w->nvof_flow_pitch,
                    (int)w->nvof_reverse_flow_pitch,
                    w->nvof_grid,
                    (float)a,
                    d_pipeline_motion_strength(w, motion_idx),
                    (void*)w->cu_stream_b);
                src = w->interp_final_rgba;
            }
        }
        if (run_manifest_output_frame_from_model_index(c, w, req,
                src, motion->input_w, motion->input_h, next_model,
                *next_out_pts, n_in + (int)*next_out_pts,
                have_prev_final, 0, n_out, bytes_out) < 0) return -1;
        (*next_out_pts)++;
    }
    CHECK_CU(cuMemcpyDtoDAsync(w->d_pipeline_rgba_b, w->d_pipeline_rgba_a,
                               (size_t)motion->input_w * motion->input_h * 4,
                               w->cu_stream_b), "intermediateMotionCopy");
    update_live_audio_encode_limit(c, curr_src_s);
    if (encode_audio_fifo(c, bytes_out, 0) < 0) return -1;
    CHECK_CU(cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                               w->pre_vsr_rgba_bytes, w->cu_stream),
             "intermediatePrevPreCopy");
    CHECK_CU(cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba,
                               w->vsr_out_bytes, w->cu_stream_b),
             "intermediatePrevFinalCopy");
    return 1;
bail:
    return -1;
}

static void d_pipeline_finish_intermediate_accept(
    BakeCtx* c, AVFrame* in_frame, int* n_in,
    int* have_clock, double* prev_clock, double curr_src_s,
    int use_pts_video_clock, double* live_clock_s,
    int64_t* prev_video_pts_us, int64_t in_pts_us,
    int n_out, int64_t next_out_pts, long long dropped,
    double interval_s, double out_fps, long long bytes_out, double t_loop) {
    *have_clock = 1;
    *prev_clock = curr_src_s;
    if (use_pts_video_clock) *live_clock_s = curr_src_s;
    if (*prev_video_pts_us == INT64_MIN || in_pts_us > *prev_video_pts_us) *prev_video_pts_us = in_pts_us;
    av_frame_unref(in_frame);
    (*n_in)++;
    log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                        next_out_pts - 1, dropped, 0,
                        (long long)fmax(0.0, floor(interval_s * out_fps) - 1.0));
    maybe_log_pipeline_progress(c, *n_in, n_out, bytes_out, in_pts_us, t_loop);
}

static int d_pipeline_run_intermediate_motion_and_finish(
    BakeCtx* c, BakeWorker* w, const BakeRequest* req, AVFrame* in_frame,
    double prev_src_s, double curr_src_s, double out_fps,
    int* n_in, int have_prev_final, int64_t* next_out_pts,
    int* n_out, long long* bytes_out, int* have_clock, double* prev_clock,
    int use_pts_video_clock, double* live_clock_s, int64_t* prev_video_pts_us,
    int64_t in_pts_us, long long dropped, double interval_s, double t_loop) {
    int handled = d_pipeline_run_intermediate_motion_interval(c, w, req,
        prev_src_s, curr_src_s, out_fps, *n_in, have_prev_final,
        next_out_pts, n_out, bytes_out);
    if (handled <= 0) return handled;
    d_pipeline_finish_intermediate_accept(c, in_frame, n_in, have_clock,
        prev_clock, curr_src_s, use_pts_video_clock, live_clock_s,
        prev_video_pts_us, in_pts_us, *n_out, *next_out_pts, dropped,
        interval_s, out_fps, *bytes_out, t_loop);
    return 1;
}
