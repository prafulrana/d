static void apply_audio_auto_mix_guard(BakeCtx* c, float* left, float* right, int samples) {
    if (!c || !left || !right || samples <= 0) return;
    if (c->audio_eq_mode <= 0) return;
    if (c->audio_auto_gain <= 0.0f) c->audio_auto_gain = 1.0f;
    if (c->audio_balance_l <= 0.0f) c->audio_balance_l = 1.0f;
    if (c->audio_balance_r <= 0.0f) c->audio_balance_r = 1.0f;

    const float bass_alpha = 0.018f; // about 140 Hz at 48 kHz.
    float target_rms = 0.17f;
    float gain_min = 0.70f;
    float gain_max = 1.22f;
    float bass_threshold = 0.58f;
    float bass_scale = 0.55f;
    float bass_max = 0.16f;
    float width = 1.00f;
    float balance_min = 0.88f;
    float balance_max = 1.12f;
    switch (c->audio_eq_mode) {
        case 2: // Voice: dialogue forward, tame boom and wide rear-heavy mixes.
            target_rms = 0.18f; gain_min = 0.72f; gain_max = 1.18f;
            bass_threshold = 0.50f; bass_scale = 0.78f; bass_max = 0.24f;
            width = 0.84f; balance_min = 0.86f; balance_max = 1.14f;
            break;
        case 3: // Movie: broadcast cinema balance, controlled lows, normal width.
            target_rms = 0.18f; gain_min = 0.68f; gain_max = 1.24f;
            bass_threshold = 0.62f; bass_scale = 0.42f; bass_max = 0.12f;
            width = 1.04f;
            break;
        case 4: // Music: preserve bed and bass, only catch lopsided/peaky mixes.
            target_rms = 0.16f; gain_min = 0.76f; gain_max = 1.14f;
            bass_threshold = 0.70f; bass_scale = 0.24f; bass_max = 0.07f;
            width = 1.02f; balance_min = 0.92f; balance_max = 1.08f;
            break;
        case 5: // Night: lower loudness, stronger bass guard, narrower stereo image.
            target_rms = 0.13f; gain_min = 0.58f; gain_max = 1.10f;
            bass_threshold = 0.45f; bass_scale = 0.90f; bass_max = 0.28f;
            width = 0.78f; balance_min = 0.84f; balance_max = 1.16f;
            break;
        case 1:
        default:
            break;
    }
    float start_lp_l = c->audio_bass_lp_l;
    float start_lp_r = c->audio_bass_lp_r;
    float lp_l = start_lp_l;
    float lp_r = start_lp_r;
    double sum_l = 0.0;
    double sum_r = 0.0;
    double sum_m = 0.0;
    double sum_low = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < samples; i++) {
        const float l = left[i];
        const float r = right[i];
        lp_l += bass_alpha * (l - lp_l);
        lp_r += bass_alpha * (r - lp_r);
        const float low = (lp_l + lp_r) * 0.5f;
        sum_l += (double)l * l;
        sum_r += (double)r * r;
        sum_m += ((double)l * l + (double)r * r) * 0.5;
        sum_low += (double)low * low;
        peak = fmaxf(peak, fmaxf(fabsf(l), fabsf(r)));
    }

    const float inv_n = 1.0f / (float)fmax(samples, 1);
    const float rms = sqrtf((float)(sum_m * inv_n));
    float desired_gain = rms > 0.008f ? target_rms / fmaxf(rms, 1e-6f) : 1.0f;
    desired_gain = clamp_float(desired_gain, gain_min, gain_max);
    if (peak > 0.001f) desired_gain = fminf(desired_gain, 0.96f / peak);
    c->audio_auto_gain = c->audio_auto_gain * 0.92f + desired_gain * 0.08f;

    const float rms_l = sqrtf((float)(sum_l * inv_n));
    const float rms_r = sqrtf((float)(sum_r * inv_n));
    const float avg_lr = (rms_l + rms_r) * 0.5f;
    if (avg_lr > 0.004f) {
        const float desired_l = clamp_float(avg_lr / fmaxf(rms_l, 1e-6f), balance_min, balance_max);
        const float desired_r = clamp_float(avg_lr / fmaxf(rms_r, 1e-6f), balance_min, balance_max);
        c->audio_balance_l = c->audio_balance_l * 0.94f + desired_l * 0.06f;
        c->audio_balance_r = c->audio_balance_r * 0.94f + desired_r * 0.06f;
    }

    const float low_ratio = sqrtf((float)(sum_low / fmax(sum_m, 1e-9)));
    const float bass_cut = clamp_float((low_ratio - bass_threshold) * bass_scale, 0.0f, bass_max);
    lp_l = start_lp_l;
    lp_r = start_lp_r;
    for (int i = 0; i < samples; i++) {
        const float l = left[i];
        const float r = right[i];
        lp_l += bass_alpha * (l - lp_l);
        lp_r += bass_alpha * (r - lp_r);
        const float eq_l = (l - lp_l * bass_cut) * c->audio_auto_gain * c->audio_balance_l;
        const float eq_r = (r - lp_r * bass_cut) * c->audio_auto_gain * c->audio_balance_r;
        const float mid = (eq_l + eq_r) * 0.5f;
        const float side = (eq_l - eq_r) * 0.5f * width;
        left[i] = clamp_float(mid + side, -0.98f, 0.98f);
        right[i] = clamp_float(mid - side, -0.98f, 0.98f);
    }
    c->audio_bass_lp_l = lp_l;
    c->audio_bass_lp_r = lp_r;
    c->audio_auto_eq_blocks++;
    if ((c->audio_auto_eq_blocks % 500) == 0) {
        fprintf(stderr,
                "[d_native_processor] audio auto-mix guard eq_mode=%d gain=%.3f balance_l=%.3f balance_r=%.3f bass_cut=%.3f width=%.2f rms=%.4f low_ratio=%.3f\n",
                c->audio_eq_mode, c->audio_auto_gain, c->audio_balance_l, c->audio_balance_r, bass_cut, width, rms, low_ratio);
    }
}

static int write_maxine_output_to_audio_fifo(BakeCtx* c, const float* const* out_planes,
                                             unsigned int samples) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) { snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_frame_alloc"); return -1; }
    frame->nb_samples = (int)samples;
    frame->format = c->audio_enc_ctx->sample_fmt;
    frame->sample_rate = c->audio_enc_ctx->sample_rate;
    if (av_channel_layout_copy(&frame->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
        av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_frame_buffer");
        return -1;
    }

    float* left = (float*)frame->extended_data[0];
    float* right = (float*)frame->extended_data[1];
    const float* src0 = out_planes[0];
    const float* src1 = c->afx.output_channels > 1 ? out_planes[1] : out_planes[0];
    if (c->audio_bed_fifo && av_audio_fifo_size(c->audio_bed_fifo) >= (int)samples) {
        if (av_audio_fifo_read(c->audio_bed_fifo, (void**)frame->extended_data, (int)samples) != (int)samples) {
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "audio_bed_fifo_read");
            return -1;
        }
        const float mix = clamp_float(0.08f + c->audio_cleanup_strength * 0.08f, 0.06f, 0.18f);
        for (unsigned int i = 0; i < samples; i++) {
            const float ml = src0[i];
            const float mr = src1[i];
            left[i] = clamp_float(left[i] * (1.0f - mix) + ml * mix, -1.0f, 1.0f);
            right[i] = clamp_float(right[i] * (1.0f - mix) + mr * mix, -1.0f, 1.0f);
        }
    } else {
        memcpy(left, src0, samples * sizeof(float));
        memcpy(right, src1, samples * sizeof(float));
    }
    apply_audio_auto_mix_guard(c, left, right, (int)samples);

    const int current = av_audio_fifo_size(c->audio_fifo);
    if (av_audio_fifo_realloc(c->audio_fifo, current + (int)samples) < 0 ||
        av_audio_fifo_write(c->audio_fifo, (void**)frame->extended_data, (int)samples) != (int)samples) {
        av_frame_free(&frame);
        snprintf(g_last_error, sizeof(g_last_error), "audio_fifo_write");
        return -1;
    }
    av_frame_free(&frame);
    return 0;
}

static int process_maxine_audio_fifo(BakeCtx* c, long long* bytes_out, int flush) {
    if (!c->afx.handle || !c->afx_input_fifo) return 0;
    const unsigned int in_samples = c->afx.input_samples;
    const unsigned int out_samples = c->afx.output_samples;
    const unsigned int out_channels = c->afx.output_channels;
    if (!in_samples || !out_samples || !out_channels) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_not_ready");
        return -1;
    }
    if (!c->afx.input_buffer || !c->afx.output_buffer ||
        c->afx.input_buffer_samples < in_samples ||
        c->afx.output_buffer_samples < (size_t)out_samples * out_channels) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_reusable_buffers_missing");
        return -1;
    }

    while (av_audio_fifo_size(c->afx_input_fifo) >= (int)in_samples ||
           (flush && av_audio_fifo_size(c->afx_input_fifo) > 0)) {
        const int available = av_audio_fifo_size(c->afx_input_fifo);
        const int read_samples = available >= (int)in_samples ? (int)in_samples : available;
        float* in = c->afx.input_buffer;
        float* out = c->afx.output_buffer;
        memset(in, 0, in_samples * sizeof(float));
        memset(out, 0, (size_t)out_samples * out_channels * sizeof(float));
        void* in_fifo_planes[1] = { in };
        if (av_audio_fifo_read(c->afx_input_fifo, in_fifo_planes, read_samples) != read_samples) {
            snprintf(g_last_error, sizeof(g_last_error), "afx_fifo_read");
            return -1;
        }

        const float* input_planes[1] = { in };
        float* output_planes[2] = { out, out + out_samples };
        NvAFX_Status status = NvAFX_Run(c->afx.handle, input_planes, output_planes,
                                        in_samples, c->afx.input_channels);
        if (status != NVAFX_STATUS_SUCCESS) {
            snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_run=%s", afx_status_name(status));
            return -1;
        }
        c->audio_maxine_runs++;
        c->audio_maxine_in_samples += in_samples;
        c->audio_maxine_out_samples += out_samples;

        if (write_maxine_output_to_audio_fifo(c, (const float* const*)output_planes, out_samples) < 0) {
            return -1;
        }
        if (encode_audio_fifo(c, bytes_out, 0) < 0) return -1;
    }
    return 0;
}

static int process_audio_packet(BakeCtx* c, AVPacket* pkt, long long* bytes_out) {
    if (!c->audio_dec_ctx || !c->audio_enc_ctx || !c->swr_ctx) return 0;
    if (pkt) c->audio_packets++;
    int ret = avcodec_send_packet(c->audio_dec_ctx, pkt);
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        snprintf(g_last_error, sizeof(g_last_error), "audio_send_packet=%d", ret);
        return -1;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) { snprintf(g_last_error, sizeof(g_last_error), "audio_dec_frame_alloc"); return -1; }
    while ((ret = avcodec_receive_frame(c->audio_dec_ctx, frame)) == 0) {
        c->audio_frames++;
        int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        int64_t pts_us = pts != AV_NOPTS_VALUE
            ? av_rescale_q(pts, c->in_fmt->streams[c->audio_stream_idx]->time_base, (AVRational){1, 1000000})
            : c->start_pts_us;
        if (pts_us < c->start_pts_us) {
            av_frame_unref(frame);
            continue;
        }
        maybe_anchor_audio_clock(c, pts_us, c->audio_maxine_active ? "asr_frame" : "program_bed_frame");

        if (!c->audio_maxine_active) {
            int dst_nb = (int)av_rescale_rnd(
                swr_get_delay(c->swr_ctx, c->audio_dec_ctx->sample_rate) + frame->nb_samples,
                c->audio_enc_ctx->sample_rate, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
            AVFrame* out_frame = av_frame_alloc();
            if (!out_frame) {
                av_frame_free(&frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_pass_frame_alloc");
                return -1;
            }
            out_frame->nb_samples = dst_nb;
            out_frame->format = c->audio_enc_ctx->sample_fmt;
            out_frame->sample_rate = c->audio_enc_ctx->sample_rate;
            if (av_channel_layout_copy(&out_frame->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
                av_frame_get_buffer(out_frame, 0) < 0) {
                av_frame_free(&out_frame);
                av_frame_free(&frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_pass_frame_buffer");
                return -1;
            }
            int converted = swr_convert(c->swr_ctx, out_frame->extended_data, dst_nb,
                                        (const uint8_t**)frame->extended_data, frame->nb_samples);
            if (converted < 0) {
                av_frame_free(&out_frame);
                av_frame_free(&frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_pass_swr_convert=%d", converted);
                return -1;
            }
            out_frame->nb_samples = converted;
            c->audio_swr_samples += converted;
            if (converted > 0) {
                if (out_frame->format == AV_SAMPLE_FMT_FLTP &&
                    out_frame->ch_layout.nb_channels >= 2 &&
                    out_frame->extended_data[0] &&
                    out_frame->extended_data[1]) {
                    apply_audio_auto_mix_guard(c,
                        (float*)out_frame->extended_data[0],
                        (float*)out_frame->extended_data[1],
                        converted);
                }
                const int current = av_audio_fifo_size(c->audio_fifo);
                if (av_audio_fifo_realloc(c->audio_fifo, current + converted) < 0 ||
                    av_audio_fifo_write(c->audio_fifo, (void**)out_frame->extended_data, converted) != converted) {
                    av_frame_free(&out_frame);
                    av_frame_free(&frame);
                    snprintf(g_last_error, sizeof(g_last_error), "audio_pass_fifo_write");
                    return -1;
                }
                if (encode_audio_fifo(c, bytes_out, 0) < 0) {
                    av_frame_free(&out_frame);
                    av_frame_free(&frame);
                    return -1;
                }
            }
            av_frame_free(&out_frame);
            av_frame_unref(frame);
            continue;
        }

        if (write_program_bed_to_fifo(c, frame) < 0) {
            av_frame_free(&frame);
            return -1;
        }

        int dst_nb = (int)av_rescale_rnd(
            swr_get_delay(c->swr_ctx, c->audio_dec_ctx->sample_rate) + frame->nb_samples,
            DPROC_AFX_INPUT_RATE, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
        AVFrame* afx_frame = av_frame_alloc();
        if (!afx_frame) {
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "afx_frame_alloc");
            return -1;
        }
        afx_frame->nb_samples = dst_nb;
        afx_frame->format = AV_SAMPLE_FMT_FLT;
        afx_frame->sample_rate = DPROC_AFX_INPUT_RATE;
        av_channel_layout_default(&afx_frame->ch_layout, DPROC_AFX_INPUT_CHANNELS);
        if (av_frame_get_buffer(afx_frame, 0) < 0) {
            av_frame_free(&afx_frame);
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "afx_frame_buffer");
            return -1;
        }
        int converted = swr_convert(c->swr_ctx, afx_frame->extended_data, dst_nb,
                                    (const uint8_t**)frame->extended_data, frame->nb_samples);
        if (converted < 0) {
            av_frame_free(&afx_frame);
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "audio_swr_convert=%d", converted);
            return -1;
        }
        afx_frame->nb_samples = converted;
        c->audio_swr_samples += converted;
        const int current = av_audio_fifo_size(c->afx_input_fifo);
        if (av_audio_fifo_realloc(c->afx_input_fifo, current + converted) < 0 ||
            av_audio_fifo_write(c->afx_input_fifo, (void**)afx_frame->extended_data, converted) != converted) {
            av_frame_free(&afx_frame);
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "afx_fifo_write");
            return -1;
        }
        av_frame_free(&afx_frame);
        av_frame_unref(frame);
        if (process_maxine_audio_fifo(c, bytes_out, 0) < 0) {
            av_frame_free(&frame);
            return -1;
        }
    }
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    snprintf(g_last_error, sizeof(g_last_error), "audio_receive_frame=%d", ret);
    return -1;
}

static int flush_audio(BakeCtx* c, long long* bytes_out) {
    if (!c->audio_dec_ctx || !c->audio_enc_ctx) return 0;
    if (process_audio_packet(c, NULL, bytes_out) < 0) return -1;
    if (c->swr_ctx && !c->audio_maxine_active) {
        int delayed = (int)av_rescale_rnd(
            swr_get_delay(c->swr_ctx, c->audio_dec_ctx->sample_rate),
            c->audio_enc_ctx->sample_rate, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
        if (delayed > 0) {
            AVFrame* out_frame = av_frame_alloc();
            if (!out_frame) { snprintf(g_last_error, sizeof(g_last_error), "audio_pass_flush_frame_alloc"); return -1; }
            out_frame->nb_samples = delayed;
            out_frame->format = c->audio_enc_ctx->sample_fmt;
            out_frame->sample_rate = c->audio_enc_ctx->sample_rate;
            if (av_channel_layout_copy(&out_frame->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
                av_frame_get_buffer(out_frame, 0) < 0) {
                av_frame_free(&out_frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_pass_flush_frame_buffer");
                return -1;
            }
            int converted = swr_convert(c->swr_ctx, out_frame->extended_data, delayed, NULL, 0);
            if (converted < 0) {
                av_frame_free(&out_frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_pass_swr_flush=%d", converted);
                return -1;
            }
            if (converted > 0) {
                out_frame->nb_samples = converted;
                if (out_frame->format == AV_SAMPLE_FMT_FLTP &&
                    out_frame->ch_layout.nb_channels >= 2 &&
                    out_frame->extended_data[0] &&
                    out_frame->extended_data[1]) {
                    apply_audio_auto_mix_guard(c,
                        (float*)out_frame->extended_data[0],
                        (float*)out_frame->extended_data[1],
                        converted);
                }
                const int current = av_audio_fifo_size(c->audio_fifo);
                if (av_audio_fifo_realloc(c->audio_fifo, current + converted) < 0 ||
                    av_audio_fifo_write(c->audio_fifo, (void**)out_frame->extended_data, converted) != converted) {
                    av_frame_free(&out_frame);
                    snprintf(g_last_error, sizeof(g_last_error), "audio_pass_fifo_flush_write");
                    return -1;
                }
            }
            av_frame_free(&out_frame);
        }
        return encode_audio_fifo(c, bytes_out, 1);
    }
    if (flush_program_bed_to_fifo(c) < 0) return -1;
    if (c->swr_ctx) {
        int delayed = (int)av_rescale_rnd(
            swr_get_delay(c->swr_ctx, c->audio_dec_ctx->sample_rate),
            DPROC_AFX_INPUT_RATE, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
        if (delayed > 0) {
            AVFrame* afx_frame = av_frame_alloc();
            if (!afx_frame) { snprintf(g_last_error, sizeof(g_last_error), "audio_flush_frame_alloc"); return -1; }
            afx_frame->nb_samples = delayed;
            afx_frame->format = AV_SAMPLE_FMT_FLT;
            afx_frame->sample_rate = DPROC_AFX_INPUT_RATE;
            av_channel_layout_default(&afx_frame->ch_layout, DPROC_AFX_INPUT_CHANNELS);
            if (av_frame_get_buffer(afx_frame, 0) < 0) {
                av_frame_free(&afx_frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_flush_frame_buffer");
                return -1;
            }
            int converted = swr_convert(c->swr_ctx, afx_frame->extended_data, delayed, NULL, 0);
            if (converted < 0) {
                av_frame_free(&afx_frame);
                snprintf(g_last_error, sizeof(g_last_error), "audio_swr_flush=%d", converted);
                return -1;
            }
            const int current = av_audio_fifo_size(c->afx_input_fifo);
            if (converted > 0 &&
                (av_audio_fifo_realloc(c->afx_input_fifo, current + converted) < 0 ||
                 av_audio_fifo_write(c->afx_input_fifo, (void**)afx_frame->extended_data, converted) != converted)) {
                av_frame_free(&afx_frame);
                snprintf(g_last_error, sizeof(g_last_error), "afx_fifo_flush_write");
                return -1;
            }
            av_frame_free(&afx_frame);
        }
    }
    if (process_maxine_audio_fifo(c, bytes_out, 1) < 0) return -1;
    return encode_audio_fifo(c, bytes_out, 1);
}
