static int drain_audio_encoder(BakeCtx* c, long long* bytes_out) {
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        snprintf(g_last_error, sizeof(g_last_error), "audio_pkt_alloc");
        return -1;
    }
    int ret;
    while ((ret = avcodec_receive_packet(c->audio_enc_ctx, pkt)) == 0) {
        pkt->stream_index = c->out_audio_stream_idx;
        av_packet_rescale_ts(pkt, c->audio_enc_ctx->time_base,
                             c->out_fmt->streams[c->out_audio_stream_idx]->time_base);
        *bytes_out += pkt->size;
        c->audio_encoded_packets++;
        c->audio_bytes_out += pkt->size;
        av_interleaved_write_frame(c->out_fmt, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    snprintf(g_last_error, sizeof(g_last_error), "audio_receive_packet=%d", ret);
    return -1;
}

static int encode_audio_fifo(BakeCtx* c, long long* bytes_out, int flush) {
    if (!c->audio_enc_ctx || !c->audio_fifo) return 0;
    const int channels = c->audio_enc_ctx->ch_layout.nb_channels;
    const int frame_size = c->audio_enc_ctx->frame_size > 0 ? c->audio_enc_ctx->frame_size : 1024;

    while (av_audio_fifo_size(c->audio_fifo) >= frame_size ||
           (flush && av_audio_fifo_size(c->audio_fifo) > 0)) {
        const int available = av_audio_fifo_size(c->audio_fifo);
        const int read_samples = available >= frame_size ? frame_size : available;
        if (!flush && c->audio_pacing_enabled &&
            c->audio_next_pts + frame_size > c->audio_encode_limit_samples) {
            break;
        }
        AVFrame* frame = av_frame_alloc();
        if (!frame) { snprintf(g_last_error, sizeof(g_last_error), "audio_frame_alloc"); return -1; }
        frame->nb_samples = frame_size;
        frame->format = c->audio_enc_ctx->sample_fmt;
        frame->sample_rate = c->audio_enc_ctx->sample_rate;
        if (av_channel_layout_copy(&frame->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
            av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "audio_frame_buffer");
            return -1;
        }
        if (av_audio_fifo_read(c->audio_fifo, (void**)frame->extended_data, read_samples) != read_samples) {
            av_frame_free(&frame);
            snprintf(g_last_error, sizeof(g_last_error), "audio_fifo_read");
            return -1;
        }
        if (read_samples < frame_size) {
            av_samples_set_silence(frame->extended_data, read_samples,
                                   frame_size - read_samples, channels,
                                   c->audio_enc_ctx->sample_fmt);
        }
        frame->pts = c->audio_next_pts + c->audio_delay_samples;
        c->audio_next_pts += frame_size;
        int ret = avcodec_send_frame(c->audio_enc_ctx, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "audio_send_frame=%d", ret);
            return -1;
        }
        if (drain_audio_encoder(c, bytes_out) < 0) return -1;
    }

    if (flush) {
        int ret = avcodec_send_frame(c->audio_enc_ctx, NULL);
        if (ret < 0 && ret != AVERROR_EOF) {
            snprintf(g_last_error, sizeof(g_last_error), "audio_send_flush=%d", ret);
            return -1;
        }
        if (drain_audio_encoder(c, bytes_out) < 0) return -1;
    }
    return 0;
}

static int write_program_bed_to_fifo(BakeCtx* c, AVFrame* src_frame) {
    if (!c->audio_bed_swr_ctx || !c->audio_bed_fifo || !src_frame) return 0;
    int dst_nb = (int)av_rescale_rnd(
        swr_get_delay(c->audio_bed_swr_ctx, c->audio_dec_ctx->sample_rate) + src_frame->nb_samples,
        c->audio_enc_ctx->sample_rate, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
    AVFrame* bed = av_frame_alloc();
    if (!bed) {
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_frame_alloc");
        return -1;
    }
    bed->nb_samples = dst_nb;
    bed->format = c->audio_enc_ctx->sample_fmt;
    bed->sample_rate = c->audio_enc_ctx->sample_rate;
    if (av_channel_layout_copy(&bed->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
        av_frame_get_buffer(bed, 0) < 0) {
        av_frame_free(&bed);
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_frame_buffer");
        return -1;
    }
    int converted = swr_convert(c->audio_bed_swr_ctx, bed->extended_data, dst_nb,
                                (const uint8_t**)src_frame->extended_data,
                                src_frame->nb_samples);
    if (converted < 0) {
        av_frame_free(&bed);
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_swr_convert=%d", converted);
        return -1;
    }
    bed->nb_samples = converted;
    if (converted > 0) {
        const int current = av_audio_fifo_size(c->audio_bed_fifo);
        if (av_audio_fifo_realloc(c->audio_bed_fifo, current + converted) < 0 ||
            av_audio_fifo_write(c->audio_bed_fifo, (void**)bed->extended_data, converted) != converted) {
            av_frame_free(&bed);
            snprintf(g_last_error, sizeof(g_last_error), "audio_bed_fifo_write");
            return -1;
        }
    }
    av_frame_free(&bed);
    return 0;
}

static int flush_program_bed_to_fifo(BakeCtx* c) {
    if (!c->audio_bed_swr_ctx || !c->audio_bed_fifo) return 0;
    int delayed = (int)av_rescale_rnd(
        swr_get_delay(c->audio_bed_swr_ctx, c->audio_dec_ctx->sample_rate),
        c->audio_enc_ctx->sample_rate, c->audio_dec_ctx->sample_rate, AV_ROUND_UP);
    if (delayed <= 0) return 0;
    AVFrame* bed = av_frame_alloc();
    if (!bed) {
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_flush_frame_alloc");
        return -1;
    }
    bed->nb_samples = delayed;
    bed->format = c->audio_enc_ctx->sample_fmt;
    bed->sample_rate = c->audio_enc_ctx->sample_rate;
    if (av_channel_layout_copy(&bed->ch_layout, &c->audio_enc_ctx->ch_layout) < 0 ||
        av_frame_get_buffer(bed, 0) < 0) {
        av_frame_free(&bed);
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_flush_frame_buffer");
        return -1;
    }
    int converted = swr_convert(c->audio_bed_swr_ctx, bed->extended_data, delayed, NULL, 0);
    if (converted < 0) {
        av_frame_free(&bed);
        snprintf(g_last_error, sizeof(g_last_error), "audio_bed_swr_flush=%d", converted);
        return -1;
    }
    if (converted > 0) {
        const int current = av_audio_fifo_size(c->audio_bed_fifo);
        if (av_audio_fifo_realloc(c->audio_bed_fifo, current + converted) < 0 ||
            av_audio_fifo_write(c->audio_bed_fifo, (void**)bed->extended_data, converted) != converted) {
            av_frame_free(&bed);
            snprintf(g_last_error, sizeof(g_last_error), "audio_bed_fifo_flush_write");
            return -1;
        }
    }
    av_frame_free(&bed);
    return 0;
}

