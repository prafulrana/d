static void warm_seek_index(BakeCtx* c) {
    if (!c->in_fmt) return;
    AVStream* vs = c->in_fmt->streams[c->video_stream_idx];
    double t0 = now_seconds();
    int64_t mid = av_rescale_q(30 * AV_TIME_BASE,
                                (AVRational){1, AV_TIME_BASE}, vs->time_base);
    if (av_seek_frame(c->in_fmt, c->video_stream_idx, mid, AVSEEK_FLAG_BACKWARD) >= 0) {
        av_seek_frame(c->in_fmt, c->video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(c->dec_ctx);
    }
    fprintf(stderr, "[d_native_processor] warm_seek_index %.2fs\n", now_seconds() - t0);
}

static int graph_terminal_output_fps(const BakeWorker* w, int fallback) {
    if (!w || w->model_stage_count <= 0) return fallback;
    int last_fps = 0;
    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (s->output_fps > 0) last_fps = s->output_fps;
        if (s->output_clock_owner && s->output_fps > 0) return s->output_fps;
        if (strcmp(s->timing_role, DPIPELINE_TIMING_ROLE_OUTPUT_CLOCK_OWNER) == 0 && s->output_fps > 0) {
            return s->output_fps;
        }
    }
    return last_fps > 0 ? last_fps : fallback;
}

static int open_output(BakeCtx* c, BakeWorker* w, const BakeRequest* req) {
    if (!w) BAIL("worker_null");
    const char* output_url = (req->output_url && req->output_url[0]) ? req->output_url : "pipe:1";
    int output_is_rtsp = strncasecmp(output_url, "rtsp://", 7) == 0 || strncasecmp(output_url, "rtsps://", 8) == 0;
    const char* output_format = (req->output_format && req->output_format[0])
        ? req->output_format
        : (output_is_rtsp ? "rtsp" : "mpegts");
    // Encoded packet boundary only. Frames remain CUDA-resident through NVENC;
    // this sink receives HEVC/AAC packets, never raw RGB/YUV frames.
    int r = avformat_alloc_output_context2(&c->out_fmt, NULL, output_format, output_is_rtsp ? output_url : NULL);
    if (r < 0 || !c->out_fmt) BAIL("output_alloc");
    c->out_fmt->flush_packets = 1;
    c->out_fmt->max_interleave_delta = 100000;

    const AVCodec* enc = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!enc) BAIL("no_hevc_nvenc");
    c->enc_ctx = avcodec_alloc_context3(enc);
    if (!c->enc_ctx) BAIL("enc_ctx_alloc");

    c->enc_ctx->codec_id = AV_CODEC_ID_HEVC;
    c->enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    c->enc_ctx->width = BAKE_OUTPUT_WIDTH;
    c->enc_ctx->height = BAKE_OUTPUT_HEIGHT;
    c->enc_ctx->pix_fmt = AV_PIX_FMT_CUDA;
    c->enc_ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    // Smooth live output: keep source-fps for interpolation math, but encode
    // at the requested output rate (60fps by default, 120fps experimental).
    AVRational src_fps = c->in_fmt->streams[c->video_stream_idx]->avg_frame_rate;
    if (src_fps.num <= 0 || src_fps.den <= 0) src_fps = c->in_fmt->streams[c->video_stream_idx]->r_frame_rate;
    double src_fps_f = (src_fps.num > 0 && src_fps.den > 0) ? (double)src_fps.num / (double)src_fps.den : 0.0;
    if (src_fps_f < 12.0 || src_fps_f > (double)DPIPELINE_FPS_MAX) {
        src_fps_f = 24.0;
    }
    int requested_out_fps = req->fps > 0 ? req->fps : DPIPELINE_FPS_DEFAULT;
    int out_fps = graph_terminal_output_fps(w, requested_out_fps);
    if (out_fps != requested_out_fps) {
        fprintf(stderr,
                "[d_native_processor] graph_terminal_fps_override request_fps=%d graph_fps=%d source=d_pipeline\n",
                requested_out_fps, out_fps);
    }
    if (out_fps < 24) out_fps = 24;
    if (out_fps > DPIPELINE_FPS_MAX) out_fps = DPIPELINE_FPS_MAX;
    c->audio_cleanup_strength = clamp_float(req->audio_cleanup_strength, 0.0f, 1.0f);
    c->audio_superres_mode = req->audio_superres_mode;
    c->audio_passthrough = req->audio_passthrough ? 1 : 0;
    c->audio_eq_mode = req->audio_eq_mode;
    c->live_clock_mode = req->live_clock_mode ? 1 : 0;
    c->audio_pacing_enabled = req->audio_pacing_mode == DPROC_AUDIO_PACING_VIDEO_GATED ? 1 : 0;
    c->max_audio_lead_ms = req->max_audio_lead_ms;
    if (c->audio_pacing_enabled && c->max_audio_lead_ms <= 0) c->max_audio_lead_ms = 750;
    if (c->max_audio_lead_ms < 0) c->max_audio_lead_ms = 0;
    if (c->max_audio_lead_ms > 2000) c->max_audio_lead_ms = 2000;
    c->max_av_delta_ms = req->max_av_delta_ms;
    if (c->max_av_delta_ms < 0) c->max_av_delta_ms = 0;
    if (c->max_av_delta_ms > 5000) c->max_av_delta_ms = 5000;
    c->audio_auto_gain = 1.0f;
    c->audio_balance_l = 1.0f;
    c->audio_balance_r = 1.0f;
    AVRational enc_fps = (AVRational){ out_fps, 1 };
    c->src_fps_f = src_fps_f;
    c->out_fps_f = (double)out_fps;
    apply_audio_delay(c, req->audio_delay_ms);
    // FRUC is for real frame-rate uplift. Do not synthesize pointless
    // near-duplicate endpoint frames when source cadence already matches the
    // requested output cadence; do synthesize 15/24/30 fps sources to 60/120.
    c->nvof_enabled = ((double)out_fps > src_fps_f + 0.5) ? 1 : 0;
    c->cadence_lock = (!c->nvof_enabled && fabs((double)out_fps - src_fps_f) <= 0.75) ? 1 : 0;
    c->live_pace_initialized = 0;
    c->live_pace_wall_start_s = 0.0;
    const int cushion_ms = req->live_output_cushion_ms >= 0
        ? req->live_output_cushion_ms
        : env_int_clamped("DPROC_LIVE_OUTPUT_CUSHION_MS", 3000, 0, 60000);
    c->live_pace_cushion_s = (double)cushion_ms / 1000.0;
    c->live_pace_slept_s = 0.0;
    c->live_pace_sleep_count = 0;
    const int video_delay_ms = env_int_clamped("DPROC_VIDEO_SYNC_DELAY_MS", 0, 0, 1000);
    c->video_sync_delay_pts = (int64_t)llround(((double)video_delay_ms / 1000.0) * (double)out_fps);
    c->enc_ctx->time_base = av_inv_q(enc_fps);
    c->enc_ctx->framerate = enc_fps;
    c->enc_ctx->gop_size = out_fps;
    c->enc_ctx->keyint_min = out_fps;
    const int hevc_bframes = req->is_live ? 0 : 2;
    const int hevc_lookahead = req->is_live ? 0 : 16;
    fprintf(stderr, "[d_native_processor] source fps=%.3f output fps=%d interpolation=%s cadence_lock=%d output_queue_ms=%d live_cushion_s=%.1f video_clock_mode=%s audio_pacing=%s max_audio_lead_ms=%d max_av_delta_ms=%d codec=hevc_nvenc gop=%d bframes=%d lookahead=%d idr=forced aud=1 video_sync_delay_ms=%d delay_pts=%lld\n",
            src_fps_f, out_fps,
            c->nvof_enabled ? "nvof_fruc_uniform_pts" : "nvof_loaded_no_synthetic_needed",
            c->cadence_lock,
            cushion_ms,
            c->live_pace_cushion_s,
            c->live_clock_mode ? "pts-gap-squash" : "source-pts",
            c->audio_pacing_enabled ? "video-gated" : "source-pts",
            c->max_audio_lead_ms,
            c->max_av_delta_ms,
            c->enc_ctx->gop_size, hevc_bframes, hevc_lookahead, video_delay_ms, (long long)c->video_sync_delay_pts);
    c->enc_ctx->bit_rate = req->bitrate_bps;
    c->enc_ctx->rc_max_rate = req->max_bitrate_bps;
    c->enc_ctx->rc_buffer_size = req->max_bitrate_bps;
    c->enc_ctx->max_b_frames = hevc_bframes;
    if (c->out_fmt->oformat->flags & AVFMT_GLOBALHEADER) c->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(c->enc_ctx->priv_data, "preset", "p4", 0);
    av_opt_set(c->enc_ctx->priv_data, "tune", "hq", 0);
    av_opt_set(c->enc_ctx->priv_data, "rc", "vbr", 0);
    av_opt_set(c->enc_ctx->priv_data, "profile", "main", 0);
    av_opt_set_int(c->enc_ctx->priv_data, "bf", hevc_bframes, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "spatial_aq", 1, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "temporal_aq", 1, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "aq-strength", 10, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "rc-lookahead", hevc_lookahead, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "forced-idr", 1, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "strict_gop", 1, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "aud", 1, 0);
    av_opt_set_int(c->enc_ctx->priv_data, "repeat-headers", 1, 0);

    // Build a fresh hw_frames_ctx for the encoder at 4K NV12.
    c->enc_frames_ref = av_hwframe_ctx_alloc(c->hw_device_ref);
    if (!c->enc_frames_ref) BAIL("enc_frames_alloc");
    AVHWFramesContext* fctx = (AVHWFramesContext*)c->enc_frames_ref->data;
    fctx->format = AV_PIX_FMT_CUDA;
    fctx->sw_format = AV_PIX_FMT_NV12;
    fctx->width = BAKE_OUTPUT_WIDTH;
    fctx->height = BAKE_OUTPUT_HEIGHT;
    fctx->initial_pool_size = 12;
    if (av_hwframe_ctx_init(c->enc_frames_ref) < 0) BAIL("enc_frames_init");
    c->enc_ctx->hw_frames_ctx = av_buffer_ref(c->enc_frames_ref);

    if (avcodec_open2(c->enc_ctx, enc, NULL) < 0) BAIL("enc_open");

    AVStream* st = avformat_new_stream(c->out_fmt, NULL);
    if (!st) BAIL("out_stream");
    avcodec_parameters_from_context(st->codecpar, c->enc_ctx);
    st->time_base = c->enc_ctx->time_base;
    st->codecpar->codec_tag = MKTAG('h','v','c','1');
    c->out_video_stream_idx = st->index;

    if (c->audio_dec_ctx) {
        const AVCodec* aenc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!aenc) BAIL("no_aac_encoder");
        c->audio_enc_ctx = avcodec_alloc_context3(aenc);
        if (!c->audio_enc_ctx) BAIL("audio_enc_alloc");

        c->audio_enc_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
        c->audio_enc_ctx->codec_id = AV_CODEC_ID_AAC;
        c->audio_enc_ctx->sample_rate = 48000;
        c->audio_enc_ctx->bit_rate = 256000;
        c->audio_enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (aenc->sample_fmts) {
            c->audio_enc_ctx->sample_fmt = aenc->sample_fmts[0];
            for (const enum AVSampleFormat* p = aenc->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++) {
                if (*p == AV_SAMPLE_FMT_FLTP) {
                    c->audio_enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
                    break;
                }
            }
        }
        av_channel_layout_default(&c->audio_enc_ctx->ch_layout, 2);
        c->audio_enc_ctx->time_base = (AVRational){1, c->audio_enc_ctx->sample_rate};
        if (c->out_fmt->oformat->flags & AVFMT_GLOBALHEADER) c->audio_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(c->audio_enc_ctx, aenc, NULL) < 0) BAIL("audio_enc_open");

        AVStream* dst_audio = avformat_new_stream(c->out_fmt, NULL);
        if (!dst_audio) BAIL("out_audio_stream");
        avcodec_parameters_from_context(dst_audio->codecpar, c->audio_enc_ctx);
        dst_audio->codecpar->codec_tag = 0;
        dst_audio->time_base = c->audio_enc_ctx->time_base;
        c->out_audio_stream_idx = dst_audio->index;

        c->audio_maxine_active = should_use_maxine_audio(c, req);
        if (c->audio_maxine_active) {
            c->audio_passthrough = 0;
        }
        if (!c->audio_maxine_active) {
            c->audio_passthrough = 1;
        }
        w->audio_maxine_active = c->audio_maxine_active;
        w->audio_passthrough_active = c->audio_passthrough;
        bake_write_runtime_state(w,
            w->vsr_input_width > 0 ? w->vsr_input_width : c->dec_ctx->width,
            w->vsr_input_height > 0 ? w->vsr_input_height : c->dec_ctx->height,
            w->graph_model_input_h);

        if (!c->audio_maxine_active) {
            if (swr_alloc_set_opts2(&c->swr_ctx,
                                    &c->audio_enc_ctx->ch_layout,
                                    c->audio_enc_ctx->sample_fmt,
                                    c->audio_enc_ctx->sample_rate,
                                    &c->audio_dec_ctx->ch_layout,
                                    c->audio_dec_ctx->sample_fmt,
                                    c->audio_dec_ctx->sample_rate,
                                    0, NULL) < 0) {
                BAIL("swr_alloc_pass_aac");
            }
        } else {
            if (init_maxine_audio_chain(c) < 0) goto bail;
            if (swr_alloc_set_opts2(&c->audio_bed_swr_ctx,
                                    &c->audio_enc_ctx->ch_layout,
                                    c->audio_enc_ctx->sample_fmt,
                                    c->audio_enc_ctx->sample_rate,
                                    &c->audio_dec_ctx->ch_layout,
                                    c->audio_dec_ctx->sample_fmt,
                                    c->audio_dec_ctx->sample_rate,
                                    0, NULL) < 0) {
                BAIL("swr_alloc_program_bed");
            }
            AVChannelLayout afx_layout;
            av_channel_layout_default(&afx_layout, DPROC_AFX_INPUT_CHANNELS);
            if (swr_alloc_set_opts2(&c->swr_ctx,
                                    &afx_layout,
                                    AV_SAMPLE_FMT_FLT,
                                    DPROC_AFX_INPUT_RATE,
                                    &c->audio_dec_ctx->ch_layout,
                                    c->audio_dec_ctx->sample_fmt,
                                    c->audio_dec_ctx->sample_rate,
                                    0, NULL) < 0) {
                av_channel_layout_uninit(&afx_layout);
                BAIL("swr_alloc_maxine");
            }
            av_channel_layout_uninit(&afx_layout);
        }
        if (swr_init(c->swr_ctx) < 0) BAIL("swr_init");
        if (c->audio_bed_swr_ctx && swr_init(c->audio_bed_swr_ctx) < 0) BAIL("swr_init_program_bed");
        if (c->audio_maxine_active) {
            c->afx_input_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT,
                                                    DPROC_AFX_INPUT_CHANNELS,
                                                    c->afx.input_samples * 8);
            if (!c->afx_input_fifo) BAIL("afx_fifo_alloc");
            c->audio_bed_fifo = av_audio_fifo_alloc(c->audio_enc_ctx->sample_fmt,
                                                    c->audio_enc_ctx->ch_layout.nb_channels,
                                                    c->audio_enc_ctx->frame_size > 0 ? c->audio_enc_ctx->frame_size * 8 : 8192);
            if (!c->audio_bed_fifo) BAIL("audio_bed_fifo_alloc");
        }
        c->audio_fifo = av_audio_fifo_alloc(c->audio_enc_ctx->sample_fmt,
                                            c->audio_enc_ctx->ch_layout.nb_channels,
                                            c->audio_enc_ctx->frame_size > 0 ? c->audio_enc_ctx->frame_size * 4 : 4096);
        if (!c->audio_fifo) BAIL("audio_fifo_alloc");
        int audio_delay_ms = req->audio_delay_ms;
        if (audio_delay_ms < -1000) audio_delay_ms = -1000;
        if (audio_delay_ms > 1000) audio_delay_ms = 1000;
        c->audio_next_pts = 0;
        c->max_audio_lead_samples = c->audio_pacing_enabled
            ? (int64_t)llround(((double)c->max_audio_lead_ms / 1000.0) * (double)c->audio_enc_ctx->sample_rate)
            : 0;
        c->audio_encode_limit_samples = c->audio_pacing_enabled
            ? c->max_audio_lead_samples
            : INT64_MAX;
        c->audio_source_base_pts_us = 0;
        c->audio_clock_initialized = 0;
        c->audio_clock_resyncs = 0;
        apply_audio_delay(c, audio_delay_ms);
        if (!c->audio_maxine_active) {
            fprintf(stderr, "[d_native_processor] audio path program-bed codec=%s ch=%d sr=%d -> swr_48k_stereo + eq_mode=%d -> aac_lc ch=2 sr=48000 bitrate=256000 maxine_asr=0 mode=%s audio_pacing=%s max_audio_lead_ms=%d audio_delay_ms=%d delay_samples=%lld\n",
                    avcodec_get_name(c->audio_dec_ctx->codec_id),
                    c->audio_dec_ctx->ch_layout.nb_channels,
                    c->audio_dec_ctx->sample_rate,
                    c->audio_eq_mode,
                    audio_superres_mode_name(c->audio_superres_mode),
                    c->audio_pacing_enabled ? "video-gated" : "source-pts",
                    c->max_audio_lead_ms,
                    audio_delay_ms,
                    (long long)c->audio_delay_samples);
            if (req->audio_superres_mode == DPROC_AUDIO_SR_AUTO) {
                fprintf(stderr, "[d_native_processor] audio auto policy: full-band program audio preserved; Maxine AudioSR reserved for <=16k/narrowband or force\n");
            }
        } else {
            fprintf(stderr, "[d_native_processor] audio path codec=%s ch=%d sr=%d -> program_bed_48k_stereo + Maxine %s cleanup=%.3f+AudioSR48k sidechain mode=%s + auto_eq_mode=%d -> aac_lc ch=2 sr=48000 bitrate=256000 audio_pacing=%s max_audio_lead_ms=%d audio_delay_ms=%d delay_samples=%lld\n",
                    avcodec_get_name(c->audio_dec_ctx->codec_id),
                    c->audio_dec_ctx->ch_layout.nb_channels,
                    c->audio_dec_ctx->sample_rate,
                    c->audio_cleanup_strength <= 0.0001f ? "audiosr_no_cleanup_chain" : "entertainment_light_cleanup16k",
                    c->audio_cleanup_strength,
                    audio_superres_mode_name(c->audio_superres_mode),
                    c->audio_eq_mode,
                    c->audio_pacing_enabled ? "video-gated" : "source-pts",
                    c->max_audio_lead_ms,
                    audio_delay_ms,
                    (long long)c->audio_delay_samples);
        }
    } else {
        fprintf(stderr, "[d_native_processor] audio path disabled: no source audio stream or decoder unavailable\n");
    }

    AVDictionary* mux_opts = NULL;
    if (output_is_rtsp) {
        av_dict_set(&mux_opts, "rtsp_transport", "tcp", 0);
        // MediaMTX accepts RTP payloads up to 1440 bytes. FFmpeg's RTSP
        // muxer defaults to 1472, which makes MediaMTX repacketize every
        // oversized packet ("RTP packets are too big") and burns avoidable
        // CPU on the edge. Keep D under the edge MTU so packets pass through.
        av_dict_set(&mux_opts, "pkt_size", "1400", 0);
        av_dict_set(&mux_opts, "muxdelay", "0.1", 0);
    } else {
        #ifdef F_SETPIPE_SZ
        (void)fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 4 * 1024 * 1024);
        #endif
        if (!(c->out_fmt->oformat->flags & AVFMT_NOFILE) &&
            avio_open(&c->out_fmt->pb, output_url, AVIO_FLAG_WRITE) < 0) {
            BAIL("avio_open_output");
        }
        av_dict_set(&mux_opts, "mpegts_flags", "resend_headers", 0);
        av_dict_set(&mux_opts, "pat_period", "0.10", 0);
        av_dict_set(&mux_opts, "sdt_period", "0.50", 0);
        av_dict_set(&mux_opts, "pcr_period", "20", 0);
    }
    int wh = avformat_write_header(c->out_fmt, &mux_opts);
    av_dict_free(&mux_opts);
    if (wh < 0) BAIL("write_header");
    c->output_header_written = 1;
    fprintf(stderr,
            "[d_native_processor] write_header ok format=%s url=%s video_stream=%d audio_stream=%d output=%dx%d fps=%.0f bitrate=%d max=%d\n",
            output_format, output_url,
            c->out_video_stream_idx, c->out_audio_stream_idx,
            BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT, c->out_fps_f,
            req->bitrate_bps, req->max_bitrate_bps);
    return 0;
bail:
    return -1;
}

static void close_ctx(BakeCtx* c) {
    if (!c) return;
    if (c->out_fmt) {
        if (c->output_header_written) av_write_trailer(c->out_fmt);
        if (!(c->out_fmt->oformat->flags & AVFMT_NOFILE) && c->out_fmt->pb) {
            avio_closep(&c->out_fmt->pb);
        }
        avformat_free_context(c->out_fmt);
        c->out_fmt = NULL;
    }
    if (c->audio_fifo) { av_audio_fifo_free(c->audio_fifo); c->audio_fifo = NULL; }
    if (c->audio_bed_fifo) { av_audio_fifo_free(c->audio_bed_fifo); c->audio_bed_fifo = NULL; }
    if (c->afx_input_fifo) { av_audio_fifo_free(c->afx_input_fifo); c->afx_input_fifo = NULL; }
    destroy_maxine_audio_chain(&c->afx);
    if (c->swr_ctx) swr_free(&c->swr_ctx);
    if (c->audio_bed_swr_ctx) swr_free(&c->audio_bed_swr_ctx);
    if (c->audio_enc_ctx) avcodec_free_context(&c->audio_enc_ctx);
    if (c->audio_dec_ctx) avcodec_free_context(&c->audio_dec_ctx);
    if (c->video_enc_frame) av_frame_free(&c->video_enc_frame);
    if (c->video_enc_packet) av_packet_free(&c->video_enc_packet);
    if (c->enc_ctx) avcodec_free_context(&c->enc_ctx);
    if (c->dec_ctx) avcodec_free_context(&c->dec_ctx);
    if (c->enc_frames_ref) av_buffer_unref(&c->enc_frames_ref);
    if (c->in_fmt) avformat_close_input(&c->in_fmt);
}
