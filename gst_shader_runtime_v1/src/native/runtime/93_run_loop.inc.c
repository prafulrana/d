static int run_pipeline(BakeCtx* c, BakeWorker* w, const BakeRequest* req, BakeResult* res, double t0) {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  in_frame = av_frame_alloc();
    int n_in = 0, n_out = 0;
    long long bytes_out = 0;
    int eof = 0;
    int rc = 0;
    int have_prev = 0;
    int64_t next_out_pts = 0;
    int64_t first_video_pts_us = INT64_MIN;
    int64_t prev_video_pts_us = INT64_MIN;
    double sasta_live_video_clock_s = 0.0;
    long long sasta_live_video_discontinuities = 0;
    long long sasta_live_video_pts_jitter = 0;
    long long sasta_live_video_backward = 0;
    const double out_fps = c->out_fps_f > 0.0 ? c->out_fps_f : 60.0;
    const double raw_src_fps = c->src_fps_f > 0.0 ? c->src_fps_f : 24.0;
    double d_pipeline_stage_clock_fps = 0.0;
    int d_pipeline_stage_clock_index = -1;
    int d_pipeline_output_clock_index = -1;
    const char* d_pipeline_stage_clock_policy = "none";
    long long d_pipeline_source_seq = 0;
    long long d_pipeline_stage_seq = 0;
    long long d_pipeline_dropped = 0;
    double d_pipeline_first_accepted_clock_s = 0.0;
    double d_pipeline_prev_accepted_clock_s = 0.0;
    int d_pipeline_have_accepted_clock = 0;
    const int d_pipeline_framegen_index = d_pipeline_frame_generation_owner_index(w);
    const int d_pipeline_intermediate_framegen_index = d_pipeline_intermediate_motion_index(w);
    d_pipeline_stage_clock_fps = d_pipeline_source_gate_fps(w, raw_src_fps, &d_pipeline_stage_clock_index,
        &d_pipeline_output_clock_index, &d_pipeline_stage_clock_policy);
    const int d_pipeline_stage_clock_enabled = d_pipeline_stage_clock_fps > 0.0;
    if (w && w->model_stage_count > 0) {
        c->nvof_enabled = d_pipeline_framegen_index >= 0 ? 1 : 0;
        c->cadence_lock = c->nvof_enabled ? 0 : c->cadence_lock;
    }
    if (d_pipeline_stage_clock_enabled) {
        fprintf(stderr,
                "[d_native_processor] d_pipeline cadence owner stage_clock_fps=%.3f raw_src_fps=%.3f output_fps=%.3f stage_index=%d output_clock_owner_index=%d framegen_owner_index=%d policy=%s graph_nvof=%d\n",
                d_pipeline_stage_clock_fps, raw_src_fps, out_fps, d_pipeline_stage_clock_index,
                d_pipeline_output_clock_index, d_pipeline_framegen_index,
                d_pipeline_stage_clock_policy, c->nvof_enabled ? 1 : 0);
    }
    if (req->live_clock_mode == 1) {
        fprintf(stderr, "[d_native_processor] source_receiver mode=normalize-live-decode-order source_fps=%.3f output_fps=%.3f pts_policy=upstream-evidence-only downstream_clock=monotonic\n",
                raw_src_fps, out_fps);
    }
    BakeRequest live_req = *req;
    clamp_runtime_tuning(&live_req);
    maybe_reload_live_tuning(c, &live_req, 1);
    maybe_open_perf_ring(c, req);
    double t_loop = now_seconds();
    const int live_read_resilient = req->is_live || looks_like_live_input(req->url);
    const double live_read_retry_budget_s =
        env_double_clamped("DPROC_LIVE_READ_RETRY_BUDGET_S", 45.0, 1.0, 300.0);
    const int live_read_retry_max =
        env_int_clamped("DPROC_LIVE_READ_RETRY_MAX", 90, 1, 10000);
    int live_read_failures = 0;
    double live_read_first_failure_s = 0.0;

    while (!eof) {
        int rr = av_read_frame(c->in_fmt, pkt);
        if (rr == AVERROR_EOF && live_read_resilient) {
            if (live_read_first_failure_s <= 0.0) live_read_first_failure_s = now_seconds();
            live_read_failures++;
            const double age_s = now_seconds() - live_read_first_failure_s;
            if (live_read_failures <= live_read_retry_max && age_s <= live_read_retry_budget_s) {
                if (live_read_failures <= 8 || (live_read_failures % 30) == 0) {
                    fprintf(stderr,
                            "[d_native_processor] live_read_retry eof failures=%d age_s=%.3f budget_s=%.3f\n",
                            live_read_failures, age_s, live_read_retry_budget_s);
                }
                av_packet_unref(pkt);
                usleep((useconds_t)(100000 + (live_read_failures > 10 ? 400000 : live_read_failures * 25000)));
                continue;
            }
            snprintf(g_last_error, sizeof(g_last_error), "av_read_frame_live_eof_timeout");
            rc = -1;
            goto done;
        } else if (rr == AVERROR_EOF) {
            eof = 1; av_packet_unref(pkt); pkt->data = NULL; pkt->size = 0;
        } else if (rr < 0 && live_read_resilient) {
            if (live_read_first_failure_s <= 0.0) live_read_first_failure_s = now_seconds();
            live_read_failures++;
            const double age_s = now_seconds() - live_read_first_failure_s;
            if (live_read_failures <= live_read_retry_max && age_s <= live_read_retry_budget_s) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(rr, errbuf, sizeof(errbuf));
                if (live_read_failures <= 8 || (live_read_failures % 30) == 0) {
                    fprintf(stderr,
                            "[d_native_processor] live_read_retry err=%s failures=%d age_s=%.3f budget_s=%.3f\n",
                            errbuf[0] ? errbuf : "unknown",
                            live_read_failures, age_s, live_read_retry_budget_s);
                }
                av_packet_unref(pkt);
                usleep((useconds_t)(100000 + (live_read_failures > 10 ? 400000 : live_read_failures * 25000)));
                continue;
            }
            snprintf(g_last_error, sizeof(g_last_error), "av_read_frame_live_retry_exhausted");
            rc = -1;
            goto done;
        } else if (rr < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "av_read_frame");
            rc = -1;
            goto done;
        } else if (live_read_failures > 0) {
            fprintf(stderr,
                    "[d_native_processor] live_read_recovered failures=%d outage_s=%.3f\n",
                    live_read_failures,
                    live_read_first_failure_s > 0.0 ? now_seconds() - live_read_first_failure_s : 0.0);
            live_read_failures = 0;
            live_read_first_failure_s = 0.0;
        }
        if (!eof && pkt->stream_index == c->audio_stream_idx && c->out_audio_stream_idx >= 0) {
            double ta = now_seconds();
            int audio_rc = process_audio_packet(c, pkt, &bytes_out);
            if (audio_rc < 0) {
                rc = -1;
                av_packet_unref(pkt);
                goto done;
            }
            c->stage_audio_s += now_seconds() - ta;
            av_packet_unref(pkt);
            continue;
        }
        if (!eof && pkt->stream_index != c->video_stream_idx) { av_packet_unref(pkt); continue; }

        int sr = avcodec_send_packet(c->dec_ctx, pkt);
        av_packet_unref(pkt);
        if (sr < 0 && sr != AVERROR_EOF) { snprintf(g_last_error, sizeof(g_last_error), "send_packet"); rc = -1; goto done; }

        while (avcodec_receive_frame(c->dec_ctx, in_frame) == 0) {
            int64_t in_pts_us = av_rescale_q(in_frame->pts, c->in_fmt->streams[c->video_stream_idx]->time_base,
                                             (AVRational){1, 1000000});
            if (in_pts_us < c->start_pts_us) { av_frame_unref(in_frame); continue; }
            if (req->duration_seconds > 0.0 &&
                in_pts_us > c->start_pts_us + (int64_t)(req->duration_seconds * 1000000.0)) {
                eof = 1; av_frame_unref(in_frame); break;
            }
            if (prev_video_pts_us != INT64_MIN && in_pts_us <= prev_video_pts_us) {
                if (req->live_clock_mode == 1) {
                    sasta_live_video_backward++;
                    if (sasta_live_video_backward <= 8 || (sasta_live_video_backward % 120) == 0) {
                        fprintf(stderr, "[d_native_processor] video clock source-pts-backward-ignored source_pts_us=%lld prev_pts_us=%lld normalized_clock_s=%.3f backward=%lld\n",
                                (long long)in_pts_us, (long long)prev_video_pts_us, sasta_live_video_clock_s, sasta_live_video_backward);
                    }
                } else {
                    av_frame_unref(in_frame);
                    continue;
                }
            }
            if (prev_video_pts_us != INT64_MIN && in_pts_us <= prev_video_pts_us && req->live_clock_mode != 1) {
                av_frame_unref(in_frame);
                continue;
            }
            const long long source_seq = d_pipeline_source_seq++;
            if (d_pipeline_stage_clock_enabled) {
                const long long before = (long long)floor(((double)source_seq * d_pipeline_stage_clock_fps) / raw_src_fps + 1e-9),
                                after = (long long)floor(((double)(source_seq + 1) * d_pipeline_stage_clock_fps) / raw_src_fps + 1e-9);
                if (source_seq > 0 && after <= before) {
                    d_pipeline_dropped++;
                    if (w && d_pipeline_stage_clock_index >= 0 && d_pipeline_stage_clock_index < w->model_stage_count) {
                        w->model_stages[d_pipeline_stage_clock_index].cadence_dropped++;
                    }
                    av_frame_unref(in_frame);
                    continue;
                }
            }
            const long long accepted_stage_seq = d_pipeline_stage_clock_enabled ? d_pipeline_stage_seq++ : (long long)n_in;
            double accepted_source_clock_s = d_pipeline_stage_sample_clock_s(d_pipeline_stage_clock_enabled,
                d_pipeline_stage_clock_fps, raw_src_fps, source_seq, accepted_stage_seq, n_in);
            if (!d_pipeline_have_accepted_clock) {
                d_pipeline_first_accepted_clock_s = accepted_source_clock_s;
            }
            accepted_source_clock_s -= d_pipeline_first_accepted_clock_s;
            if (accepted_source_clock_s < 0.0) accepted_source_clock_s = 0.0;
            if (first_video_pts_us == INT64_MIN) first_video_pts_us = in_pts_us;

            if (in_frame->format != AV_PIX_FMT_CUDA) { snprintf(g_last_error, sizeof(g_last_error), "decoder_not_cuda"); rc = -1; av_frame_unref(in_frame); goto done; }
            const int input_format = yuv420_input_format_for_cuda_frame(in_frame);
            if (input_format < 0) {
                enum AVPixelFormat sw = cuda_frame_sw_format(in_frame);
                snprintf(g_last_error, sizeof(g_last_error), "unsupported_cuda_sw_format=%s",
                         av_get_pix_fmt_name(sw) ? av_get_pix_fmt_name(sw) : "unknown");
                rc = -1;
                av_frame_unref(in_frame);
                goto done;
            }
            if (n_in == 0) {
                enum AVPixelFormat sw = cuda_frame_sw_format(in_frame);
                fprintf(stderr, "[d_native_processor] decoder cuda surface sw_format=%s yuv420_input_format=%d\n",
                        av_get_pix_fmt_name(sw) ? av_get_pix_fmt_name(sw) : "unknown",
                        input_format);
            }
            maybe_reload_live_tuning(c, &live_req, 0);

            if (run_source_prep_stage(c, w, &live_req, in_frame, input_format) < 0) {
                rc = -1; av_frame_unref(in_frame); goto done;
            }

            if (!have_prev) {
                if (d_pipeline_seed_or_output_first_frame(c, w, &live_req,
                                                          d_pipeline_intermediate_framegen_index,
                                                          next_out_pts++, n_in,
                                                          &n_out, &bytes_out) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                update_live_audio_encode_limit(c, (double)next_out_pts / out_fps);
                if (encode_audio_fifo(c, &bytes_out, 0) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                                  w->pre_vsr_rgba_bytes, w->cu_stream);
                cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba, w->vsr_out_bytes, w->cu_stream_b); /* no sync — downstream stages on same stream */
                have_prev = 1;
                d_pipeline_have_accepted_clock = 1;
                d_pipeline_prev_accepted_clock_s = accepted_source_clock_s;
                if (req->live_clock_mode == 1) sasta_live_video_clock_s = accepted_source_clock_s;
                if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                    prev_video_pts_us = in_pts_us;
                }
                av_frame_unref(in_frame);
                n_in++;
                log_timing_evidence(c, n_out, in_pts_us, in_pts_us,
                                    next_out_pts - 1, d_pipeline_dropped, 0, 0);
                maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
                continue;
            }

            const double src_fps = d_pipeline_stage_clock_enabled
                ? d_pipeline_stage_clock_fps
                : (c->src_fps_f > 0.0 ? c->src_fps_f : 24.0);
            const int use_pts_video_clock = (req->live_clock_mode == 1 && first_video_pts_us != INT64_MIN && prev_video_pts_us != INT64_MIN);
            double prev_src_s = d_pipeline_have_accepted_clock
                ? d_pipeline_prev_accepted_clock_s
                : fmax(0.0, accepted_source_clock_s - (1.0 / src_fps));
            double curr_src_s = accepted_source_clock_s;
            if (use_pts_video_clock) {
                const double expected_interval_s = fmax(1.0 / raw_src_fps, curr_src_s - prev_src_s);
                d_pipeline_log_live_pts_anomaly(in_pts_us, prev_video_pts_us,
                                                expected_interval_s,
                                                sasta_live_video_clock_s,
                                                next_out_pts,
                                                &sasta_live_video_discontinuities,
                                                &sasta_live_video_pts_jitter);
                c->source_pts_discontinuities = sasta_live_video_discontinuities;
                c->source_pts_jitter = sasta_live_video_pts_jitter;
            }
            const double interval_s = curr_src_s - prev_src_s;
            if (interval_s <= 0.0) {
                av_frame_unref(in_frame);
                continue;
            }
            if (d_pipeline_intermediate_framegen_index >= 0) {
                int handled = d_pipeline_run_intermediate_motion_and_finish(c, w, &live_req, in_frame,
                    prev_src_s, curr_src_s, out_fps, &n_in, have_prev, &next_out_pts, &n_out, &bytes_out,
                    &d_pipeline_have_accepted_clock, &d_pipeline_prev_accepted_clock_s,
                    use_pts_video_clock, &sasta_live_video_clock_s, &prev_video_pts_us,
                    in_pts_us, d_pipeline_dropped, interval_s, t_loop);
                if (handled < 0) { rc = -1; av_frame_unref(in_frame); goto done; }
                if (handled) continue;
            }
            const int source_motion_idx = d_pipeline_source_motion_index(w);
            const int final_motion_idx = d_pipeline_final_motion_index(w);
            const float source_motion_strength = d_pipeline_motion_strength(w, source_motion_idx);
            const float final_motion_strength = d_pipeline_motion_strength(w, final_motion_idx);
            const int nvof_before_upscaler = source_motion_idx >= 0;
            const int nvof_after_upscaler = final_motion_idx >= 0;
            const int temporal_enabled =
                pipeline_stage_enabled(c, STAGE_DLSAA_TEMPORAL) &&
                (live_req.temporal_strength > 0.001f || live_req.edge_stability > 0.001f);
            const int temporal_denoise_enabled =
                pipeline_stage_enabled(c, STAGE_TEMPORAL_DENOISE) &&
                w->temporal_denoise_strength > 0.001f;
            const int source_synth_needed =
                c->nvof_enabled && source_motion_idx == d_pipeline_framegen_index && source_motion_strength > 0.001f;
            const int final_synth_needed =
                c->nvof_enabled && final_motion_idx == d_pipeline_framegen_index && final_motion_strength > 0.001f;
            const int synth_needed = source_synth_needed || final_synth_needed;
            const int trt_upscaler = (w->graph_model_family_code == DPROC_MODEL_FAMILY_ESRGAN ||
                                      w->graph_model_family_code == DPROC_MODEL_FAMILY_CUGAN);
            const int temporal_flow_allowed = synth_needed || !trt_upscaler;
            const int flow_needed =
                nvof_before_upscaler &&
                (synth_needed || (temporal_flow_allowed && (temporal_enabled || temporal_denoise_enabled)));
            if (flow_needed && run_source_nvof_stage(c, w) < 0) {
                rc = -1; av_frame_unref(in_frame); goto done;
            }
            if (nvof_after_upscaler && final_synth_needed) {
                int frame_flow_ready = 0;
                if (run_manifest_render_frame(c, w, &live_req, w->pre_vsr_rgba,
                                              n_in, have_prev, &frame_flow_ready) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                if (!frame_flow_ready) {
                    if (run_output_nvof_stage(c, w) < 0) {
                        rc = -1; av_frame_unref(in_frame); goto done;
                    }
                    frame_flow_ready = 1;
                }
                while (((double)next_out_pts / out_fps) <= curr_src_s + 0.000001) {
                    const double out_s = (double)next_out_pts / out_fps;
                    double a = (out_s - prev_src_s) / interval_s;
                    if (a < 0.0) a = 0.0;
                    if (a > 1.0) a = 1.0;
                    const float alpha = (float)a;
                    CUdeviceptr out_rgba = w->final_rgba;
                    if (alpha < 0.999f) {
                        if (alpha <= 0.001f) {
                            out_rgba = w->prev_final_rgba;
                        } else {
                            launch_nvof_fruc_interpolate(
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
                                alpha,
                                final_motion_strength,
                                (void*)w->cu_stream_b
                            );
                            out_rgba = w->interp_final_rgba;
                        }
                    }
                    if (run_manifest_encode_rgba_frame(c, w, &live_req, out_rgba, next_out_pts,
                                                       &n_out, &bytes_out) < 0) {
                        rc = -1; av_frame_unref(in_frame); goto done;
                    }
                    next_out_pts++;
                }
                update_live_audio_encode_limit(c, curr_src_s);
                if (encode_audio_fifo(c, &bytes_out, 0) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                                  w->pre_vsr_rgba_bytes, w->cu_stream);
                cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba, w->vsr_out_bytes, w->cu_stream_b);
                d_pipeline_have_accepted_clock = 1;
                d_pipeline_prev_accepted_clock_s = curr_src_s;
                if (use_pts_video_clock) sasta_live_video_clock_s = curr_src_s;
                if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                    prev_video_pts_us = in_pts_us;
                }
                av_frame_unref(in_frame);
                n_in++;
                log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                                    next_out_pts - 1, d_pipeline_dropped, 0,
                                    (long long)fmax(0.0, floor(interval_s * out_fps) - 1.0));
                maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
                continue;
            }
            if (c->nvof_enabled && !flow_needed && !nvof_after_upscaler) {
                long long held_frames = 0;
                while (((double)next_out_pts / out_fps) <= curr_src_s + 0.000001) {
                    CUdeviceptr source_rgba = w->pre_vsr_rgba;
                    if (((double)next_out_pts / out_fps) < curr_src_s - 0.000001) {
                        source_rgba = w->prev_pre_vsr_rgba;
                    }
                    if (run_manifest_output_frame(c, w, &live_req, source_rgba,
                                                  next_out_pts, n_in + (int)next_out_pts,
                                                  have_prev, 0, &n_out, &bytes_out) < 0) {
                        rc = -1; av_frame_unref(in_frame); goto done;
                    }
                    next_out_pts++;
                    held_frames++;
                }
                update_live_audio_encode_limit(c, curr_src_s);
                if (encode_audio_fifo(c, &bytes_out, 0) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                                  w->pre_vsr_rgba_bytes, w->cu_stream);
                cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba, w->vsr_out_bytes, w->cu_stream_b);
                d_pipeline_have_accepted_clock = 1;
                d_pipeline_prev_accepted_clock_s = curr_src_s;
                if (use_pts_video_clock) sasta_live_video_clock_s = curr_src_s;
                if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                    prev_video_pts_us = in_pts_us;
                }
                av_frame_unref(in_frame);
                n_in++;
                log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                                    next_out_pts - 1, d_pipeline_dropped,
                                    held_frames > 1 ? held_frames - 1 : 0, 0);
                maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
                continue;
            }
            if (!c->nvof_enabled || !source_synth_needed || !flow_needed) {
                int64_t emit_pts = next_out_pts;
                if (!c->cadence_lock) {
                    int64_t target_pts = (int64_t)llround(curr_src_s * out_fps);
                    if (target_pts < next_out_pts) {
                        cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                                          w->pre_vsr_rgba_bytes, w->cu_stream);
                        d_pipeline_have_accepted_clock = 1;
                        d_pipeline_prev_accepted_clock_s = curr_src_s;
                        if (use_pts_video_clock) sasta_live_video_clock_s = curr_src_s;
                        if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                            prev_video_pts_us = in_pts_us;
                        }
                        av_frame_unref(in_frame);
                        n_in++;
                        log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                                            next_out_pts - 1, d_pipeline_dropped, 1, 0);
                        maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
                        continue;
                    }
                    if (target_pts > next_out_pts) next_out_pts = target_pts;
                    emit_pts = next_out_pts;
                }
                if (run_manifest_output_frame(c, w, &live_req, w->pre_vsr_rgba,
                                              emit_pts, n_in, have_prev,
                                              flow_needed, &n_out, &bytes_out) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                next_out_pts = emit_pts + 1;
                update_live_audio_encode_limit(c, (double)next_out_pts / out_fps);
                if (encode_audio_fifo(c, &bytes_out, 0) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                                  w->pre_vsr_rgba_bytes, w->cu_stream);
                cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba, w->vsr_out_bytes, w->cu_stream_b); /* no sync — downstream stages on same stream */
                d_pipeline_have_accepted_clock = 1;
                d_pipeline_prev_accepted_clock_s = curr_src_s;
                if (use_pts_video_clock) sasta_live_video_clock_s = curr_src_s;
                if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                    prev_video_pts_us = in_pts_us;
                }
                av_frame_unref(in_frame);
                n_in++;
                log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                                    next_out_pts - 1, d_pipeline_dropped,
                                    c->cadence_lock ? 0 : 1, 0);
                maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
                continue;
            }
            while (((double)next_out_pts / out_fps) <= curr_src_s + 0.000001) {
                const double out_s = (double)next_out_pts / out_fps;
                double a = (out_s - prev_src_s) / interval_s;
                if (a < 0.0) a = 0.0;
                if (a > 1.0) a = 1.0;
                const float alpha = (float)a;
                CUdeviceptr source_rgba = w->pre_vsr_rgba;
                if (alpha < 0.999f) {
                    if (alpha <= 0.001f) {
                        source_rgba = w->prev_pre_vsr_rgba;
                    } else {
                        launch_nvof_fruc_interpolate(
                            (void*)(uintptr_t)w->prev_pre_vsr_rgba,
                            (void*)(uintptr_t)w->pre_vsr_rgba,
                            (void*)(uintptr_t)w->nvof_flow_ptr,
                            (void*)(uintptr_t)w->nvof_reverse_flow_ptr,
                            (void*)(uintptr_t)w->interp_pre_vsr_rgba,
                            w->vsr_input_height, w->vsr_input_width,
                            w->nvof_flow_h, w->nvof_flow_w,
                            (int)w->nvof_flow_pitch,
                            (int)w->nvof_reverse_flow_pitch,
                            w->nvof_grid,
                            alpha,
                            source_motion_strength,
                            (void*)w->cu_stream_b
                        );
                        source_rgba = w->interp_pre_vsr_rgba;
                    }
                }
                if (run_manifest_output_frame(c, w, &live_req, source_rgba, next_out_pts,
                                              n_in + (int)next_out_pts, have_prev,
                                              flow_needed, &n_out, &bytes_out) < 0) {
                    rc = -1; av_frame_unref(in_frame); goto done;
                }
                next_out_pts++;
            }
            update_live_audio_encode_limit(c, curr_src_s);
            if (encode_audio_fifo(c, &bytes_out, 0) < 0) {
                rc = -1; av_frame_unref(in_frame); goto done;
            }

            cuMemcpyDtoDAsync(w->prev_pre_vsr_rgba, w->pre_vsr_rgba,
                              w->pre_vsr_rgba_bytes, w->cu_stream);
            cuMemcpyDtoDAsync(w->prev_final_rgba, w->final_rgba, w->vsr_out_bytes, w->cu_stream_b); /* no sync — downstream stages on same stream */
            d_pipeline_have_accepted_clock = 1;
            d_pipeline_prev_accepted_clock_s = curr_src_s;
            if (use_pts_video_clock) sasta_live_video_clock_s = curr_src_s;
            if (prev_video_pts_us == INT64_MIN || in_pts_us > prev_video_pts_us) {
                prev_video_pts_us = in_pts_us;
            }
            av_frame_unref(in_frame);
            n_in++;
            log_timing_evidence(c, n_out, in_pts_us, (int64_t)llround(curr_src_s * 1000000.0),
                                next_out_pts - 1, d_pipeline_dropped, 0,
                                (long long)fmax(0.0, floor(interval_s * out_fps) - 1.0));
            maybe_log_pipeline_progress(c, n_in, n_out, bytes_out, in_pts_us, t_loop);
        }
        if (eof) break;
    }
    if (flush_audio(c, &bytes_out) < 0) {
        rc = -1;
        goto done;
    }
    // Flush encoder
    avcodec_send_frame(c->enc_ctx, NULL);
    AVPacket* op = av_packet_alloc();
    while (avcodec_receive_packet(c->enc_ctx, op) == 0) {
        op->stream_index = c->out_video_stream_idx;
        if (op->duration <= 0) op->duration = 1;
        av_packet_rescale_ts(op, c->enc_ctx->time_base,
                             c->out_fmt->streams[c->out_video_stream_idx]->time_base);
        bytes_out += op->size;
        av_interleaved_write_frame(c->out_fmt, op);
        av_packet_unref(op);
        n_out++;
    }
    av_packet_free(&op);

done:
    av_frame_free(&in_frame);
    av_packet_free(&pkt);
    if (c->perf_ring_active) { perf_ring_close(&c->perf_ring); c->perf_ring_active = 0; }
    double t_end = now_seconds();
    res->ok = (rc == 0) ? 1 : 0;
    res->frames_in = n_in;
    res->frames_out = n_out;
    res->bytes_out = bytes_out;
    res->audio_packets = c->audio_packets;
    res->audio_frames = c->audio_frames;
    res->audio_maxine_runs = c->audio_maxine_runs;
    res->audio_encoded_packets = c->audio_encoded_packets;
    res->audio_bytes_out = c->audio_bytes_out;
    res->loop_seconds = t_end - t_loop;
    res->elapsed_seconds = t_end - t0;
    if (!res->ok) strncpy(res->error, g_last_error[0] ? g_last_error : "unknown", sizeof(res->error)-1);
    return rc;
}
