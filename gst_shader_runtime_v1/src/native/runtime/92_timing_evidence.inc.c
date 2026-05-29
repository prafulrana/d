static void log_timing_evidence(BakeCtx* c, int frame_index, int64_t input_pts_us,
                                int64_t stage_pts_us, int64_t output_pts,
                                long long cadence_drops, long long duplicated_frames,
                                long long synthesized_frames) {
    if (!c) return;
    if (cadence_drops > c->cadence_drops) c->cadence_drops = cadence_drops;
    if (duplicated_frames > 0) c->duplicated_frames += duplicated_frames;
    if (synthesized_frames > 0) c->synthesized_frames += synthesized_frames;
    if (frame_index > 8 && (frame_index % 120) != 0) return;
    const double output_pts_ms = c->out_fps_f > 0.0
        ? ((double)output_pts * 1000.0) / c->out_fps_f
        : -1.0;
    const double video_s = c->out_fps_f > 0.0 ? (double)output_pts / c->out_fps_f : 0.0;
    const double audio_s = (c->audio_enc_ctx && c->audio_enc_ctx->sample_rate > 0)
        ? (double)c->audio_next_pts / (double)c->audio_enc_ctx->sample_rate
        : 0.0;
    fprintf(stderr,
            "[d_native_processor] timing_evidence frame_index=%d input_pts_us=%lld stage_pts_us=%lld output_pts=%lld output_pts_ms=%.3f av_delta_s=%.6f cadence_drops=%lld duplicated_frames=%lld synthesized_frames=%lld\n",
            frame_index,
            (long long)input_pts_us,
            (long long)stage_pts_us,
            (long long)output_pts,
            output_pts_ms,
            audio_s - video_s,
            cadence_drops,
            duplicated_frames,
            synthesized_frames);
}
