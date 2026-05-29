static int finalize_vsr_frame(BakeWorker* w, const BakeRequest* req, int frame_seed) {
    launch_post_vsr_finalize_rgba(
        (void*)(uintptr_t)w->vsr_out_rgba,
        (void*)(uintptr_t)w->final_rgba,
        BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
        req->contrast, req->saturation, 1.0f / req->gamma,
        req->grain_strength, (uint32_t)frame_seed,
        req->cas_strength, req->contrast_boost,
        (void*)w->cu_stream_b
    );
    return 0;
}

static int deband_final_frame(BakeWorker* w, int frame_seed) {
    if (w->deband_strength > 0.001f) {
        launch_deband_4k_rgba(
            (void*)(uintptr_t)w->final_rgba,
            (void*)(uintptr_t)w->interp_final_rgba,
            BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
            w->deband_strength, (uint32_t)frame_seed,
            (void*)w->cu_stream_b);
        STAGE_CHECK_CU(cuMemcpyDtoDAsync(w->final_rgba, w->interp_final_rgba,
                                         w->vsr_out_bytes, w->cu_stream_b), "debandCopy");
    }
    return 0;
bail:
    return -1;
}

static int custom_shader_final_frame(BakeWorker* w, const BakeRequest* req, int frame_seed) {
    if (req && req->custom_shader_intensity > 0.001f) {
        launch_custom_shader_rgba(
            (void*)(uintptr_t)w->final_rgba,
            (void*)(uintptr_t)w->interp_final_rgba,
            BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
            req->custom_shader_intensity, (uint32_t)frame_seed,
            (void*)w->cu_stream_b);
        STAGE_CHECK_CU(cuMemcpyDtoDAsync(w->final_rgba, w->interp_final_rgba,
                                         w->vsr_out_bytes, w->cu_stream_b), "customShaderCopy");
    }
    return 0;
bail:
    return -1;
}

static int reconstruct_temporal_frame(BakeWorker* w, const BakeRequest* req) {
    if (!w || !req) return 0;
    if (req->temporal_strength <= 0.001f && req->edge_stability <= 0.001f) return 0;
    launch_temporal_reconstruct_rgba(
        (void*)(uintptr_t)w->prev_final_rgba,
        (void*)(uintptr_t)w->final_rgba,
        (void*)(uintptr_t)w->nvof_flow_ptr,
        (void*)(uintptr_t)w->nvof_reverse_flow_ptr,
        (void*)(uintptr_t)w->interp_final_rgba,
        BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
        w->nvof_flow_h, w->nvof_flow_w,
        (int)w->nvof_flow_pitch,
        (int)w->nvof_reverse_flow_pitch,
        w->nvof_grid,
        req->temporal_strength,
        req->edge_stability,
        (void*)w->cu_stream_b
    );
    STAGE_CHECK_CU(cuMemcpyDtoDAsync(w->final_rgba, w->interp_final_rgba,
                                     w->vsr_out_bytes, w->cu_stream_b), "temporalCopy");
    return 0;
bail:
    return -1;
}

static int encode_final_rgba_frame(BakeCtx* c, BakeWorker* w, const BakeRequest* req,
                                   const void* rgba, int64_t pts,
                                   int* n_out, long long* bytes_out) {
    if (!c->video_enc_frame) {
        c->video_enc_frame = av_frame_alloc();
        if (!c->video_enc_frame) {
            snprintf(g_last_error, sizeof(g_last_error), "enc_frame_alloc");
            return -1;
        }
    }
    if (!c->video_enc_packet) {
        c->video_enc_packet = av_packet_alloc();
        if (!c->video_enc_packet) {
            snprintf(g_last_error, sizeof(g_last_error), "enc_packet_alloc");
            return -1;
        }
    }
    AVFrame* enc_frame = c->video_enc_frame;
    av_frame_unref(enc_frame);
    if (av_hwframe_get_buffer(c->enc_frames_ref, enc_frame, 0) < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "hwframe_get_buffer");
        return -1;
    }
    enc_frame->width = BAKE_OUTPUT_WIDTH;
    enc_frame->height = BAKE_OUTPUT_HEIGHT;
    enc_frame->format = AV_PIX_FMT_CUDA;

    launch_rgba_to_nv12(
        rgba,
        enc_frame->data[0], enc_frame->data[1],
        BAKE_OUTPUT_HEIGHT, BAKE_OUTPUT_WIDTH,
        enc_frame->linesize[0], enc_frame->linesize[1],
        (void*)w->cu_stream_b
    );
    CUresult cs = cuEventRecord(w->ev_encode_ready, w->cu_stream_b);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "encode_event_record=%d", cs);
        av_frame_unref(enc_frame);
        return -1;
    }
    AVHWDeviceContext* hw = c->hw_device_ref ? (AVHWDeviceContext*)c->hw_device_ref->data : NULL;
    AVCUDADeviceContext* cuda = (hw && hw->hwctx) ? (AVCUDADeviceContext*)hw->hwctx : NULL;
    cs = cuStreamWaitEvent(cuda ? cuda->stream : NULL, w->ev_encode_ready, 0);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "encode_event_wait=%d", cs);
        av_frame_unref(enc_frame);
        return -1;
    }

    pace_live_output(c, req, pts);
    enc_frame->pts = pts + c->video_sync_delay_pts;
    enc_frame->duration = 1;
    int es = avcodec_send_frame(c->enc_ctx, enc_frame);
    av_frame_unref(enc_frame);
    if (es < 0) {
        snprintf(g_last_error, sizeof(g_last_error), "enc_send=%d", es);
        return -1;
    }

    AVPacket* op = c->video_enc_packet;
    av_packet_unref(op);
    int er = 0;
    while ((er = avcodec_receive_packet(c->enc_ctx, op)) == 0) {
        if (!c->first_frame_logged) {
            fprintf(stderr, "[d_native_processor] first frame out: %dx%d CUDA NV12 -> hevc_nvenc h265 bitrate=%d max=%d fps=%.0f preset=p4 live=%d first_video_packet_pts=%lld\n",
                    BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT, req->bitrate_bps,
                    req->max_bitrate_bps, c->out_fps_f, req->is_live ? 1 : 0, (long long)op->pts);
            c->first_frame_logged = 1;
        }
        op->stream_index = c->out_video_stream_idx;
        if (op->duration <= 0) op->duration = 1;
        av_packet_rescale_ts(op, c->enc_ctx->time_base,
                             c->out_fmt->streams[c->out_video_stream_idx]->time_base);
        *bytes_out += op->size;
        int wr = av_interleaved_write_frame(c->out_fmt, op);
        if (wr < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "mux_video_write=%d", wr);
            av_packet_unref(op);
            return -1;
        }
        av_packet_unref(op);
        (*n_out)++;
    }
    if (er != AVERROR(EAGAIN) && er != AVERROR_EOF) {
        snprintf(g_last_error, sizeof(g_last_error), "enc_recv=%d", er);
        return -1;
    }
    return 0;
}

void maybe_log_pipeline_progress(BakeCtx* c,
                                 int n_in,
                                 int n_out,
                                 long long bytes_out,
                                 int64_t in_pts_us,
                                 double loop_started_s) {
    double now = stage_now_seconds();
    const double video_timeline_s = c->out_fps_f > 0.0 ? (double)n_out / c->out_fps_f : 0.0;
    const double audio_timeline_s = c->audio_enc_ctx && c->audio_enc_ctx->sample_rate > 0
        ? (double)c->audio_next_pts / (double)c->audio_enc_ctx->sample_rate
        : 0.0;
    const double av_delta_s = video_timeline_s - audio_timeline_s;
    const double age = now - loop_started_s;
    const double loop_fps = age > 0.0 ? (double)n_in / age : 0.0;
    const double active_encode_s = fmax(c->stage_encode_s - c->live_pace_slept_s, 0.0);

    if (c->perf_ring_active) {
        PerfRingSlot* s = perf_ring_reserve(&c->perf_ring);
        if (s) {
            struct rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            s->ts_ns            = perf_ring_now_ns();
            s->frame_in         = (uint32_t)n_in;
            s->frame_out        = (uint32_t)n_out;
            s->in_pts_us        = in_pts_us;
            s->bytes_out        = (uint64_t)bytes_out;
            s->audio_packets    = (uint64_t)c->audio_packets;
            s->audio_frames     = (uint64_t)c->audio_frames;
            s->audio_maxine_runs= (uint64_t)c->audio_maxine_runs;
            s->audio_enc_packets= (uint64_t)c->audio_encoded_packets;
            s->audio_bytes_out  = (uint64_t)c->audio_bytes_out;
            s->loop_fps         = (float)loop_fps;
            s->video_timeline_s = (float)video_timeline_s;
            s->audio_timeline_s = (float)audio_timeline_s;
            s->av_delta_s       = (float)av_delta_s;
            s->stage_pre_s      = (float)c->stage_pre_s;
            s->stage_vsr_s      = (float)c->stage_vsr_s;
            s->stage_post_s     = (float)c->stage_post_s;
            s->stage_temporal_s = (float)c->stage_temporal_s;
            s->stage_nvof_s     = (float)c->stage_nvof_s;
            s->stage_encode_s   = (float)active_encode_s;
            s->stage_audio_s    = (float)c->stage_audio_s;
            s->cpu_user_s       = (float)((double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6);
            s->cpu_sys_s        = (float)((double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6);
            s->rss_mb           = (float)((double)ru.ru_maxrss / 1024.0);
            s->kind             = (uint16_t)PERF_KIND_FRAME;
            s->reserved         = 0;
            s->event_code       = 0;
            s->cadence_drops    = (uint64_t)c->cadence_drops;
            s->duplicated_frames= (uint64_t)c->duplicated_frames;
            s->synthesized_frames = (uint64_t)c->synthesized_frames;
            s->source_pts_jitter= (uint64_t)c->source_pts_jitter;
            s->source_pts_discontinuities = (uint64_t)c->source_pts_discontinuities;
            perf_ring_publish(&c->perf_ring);
        }
    }

    const double log_interval_s = progress_log_interval_s();
    if (log_interval_s <= 0.0 && n_in != 1) return;
    if (c->last_progress_log_s > 0.0 && now - c->last_progress_log_s < log_interval_s && n_in != 1) return;
    c->last_progress_log_s = now;
    fprintf(stderr,
            "[d_native_processor] progress video_in=%d video_out=%d bytes=%lld age_s=%.2f loop_fps=%.1f input_pts_s=%.3f video_timeline_s=%.3f audio_timeline_s=%.3f av_delta_s=%.3f audio_pkt=%lld audio_frames=%lld audio_swr=%lld audio_maxine_runs=%lld audio_maxine_in=%lld audio_maxine_out=%lld audio_enc_pkt=%lld audio_bytes=%lld stage_s audio=%.3f pre=%.3f vsr=%.3f post=%.3f temporal=%.3f nvof=%.3f encode=%.3f pace=%.3f\n",
            n_in, n_out, bytes_out, age, loop_fps,
            in_pts_us == INT64_MIN ? -1.0 : (double)in_pts_us / 1000000.0,
            video_timeline_s, audio_timeline_s, av_delta_s,
            c->audio_packets, c->audio_frames, c->audio_swr_samples,
            c->audio_maxine_runs, c->audio_maxine_in_samples,
            c->audio_maxine_out_samples, c->audio_encoded_packets,
            c->audio_bytes_out,
            c->stage_audio_s, c->stage_pre_s, c->stage_vsr_s,
            c->stage_post_s, c->stage_temporal_s, c->stage_nvof_s,
            active_encode_s, c->live_pace_slept_s);
    if (c->live_pace_sleep_count > 0 && ((long long)c->live_pace_sleep_count % 120) == 1) {
        fprintf(stderr,
                "[d_native_processor] live pace cushion_s=%.1f slept_s=%.3f sleep_count=%lld cadence_lock=%d\n",
                c->live_pace_cushion_s, c->live_pace_slept_s,
                c->live_pace_sleep_count, c->cadence_lock);
    }
}

int pipeline_stage_index(const BakeCtx* c, uint32_t id) {
    if (!c) return -1;
    for (int i = 0; i < c->stage_count; i++) {
        if (c->stages[i].id_hash == id) return i;
    }
    return -1;
}

int pipeline_stage_enabled(const BakeCtx* c, uint32_t id) {
    return pipeline_stage_index(c, id) >= 0;
}
