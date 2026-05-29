static void d_pipeline_log_live_pts_anomaly(int64_t in_pts_us,
                                            int64_t prev_video_pts_us,
                                            double expected_interval_s,
                                            double normalized_clock_s,
                                            int64_t next_out_pts,
                                            long long* discontinuities,
                                            long long* jitter_count) {
    const double max_interval_s = fmax(0.25, expected_interval_s * 3.5);
    const double pts_interval_s = (double)(in_pts_us - prev_video_pts_us) / 1000000.0;
    if (pts_interval_s > max_interval_s) {
        if (discontinuities) (*discontinuities)++;
        fprintf(stderr,
                "[d_native_processor] video clock sasta-pts-gap-squashed source_pts_us=%lld prev_pts_us=%lld raw_interval_s=%.3f used_interval_s=%.3f clock_s=%.3f old_next=%lld new_next=%lld discontinuities=%lld\n",
                (long long)in_pts_us,
                (long long)prev_video_pts_us,
                pts_interval_s,
                expected_interval_s,
                normalized_clock_s,
                (long long)next_out_pts,
                (long long)next_out_pts,
                discontinuities ? *discontinuities : 0);
        return;
    }
    if (pts_interval_s <= 0.0) return;
    const double jitter_s = fabs(pts_interval_s - expected_interval_s);
    if (jitter_s <= expected_interval_s * 0.35) return;
    if (jitter_count) (*jitter_count)++;
    if (!jitter_count || *jitter_count <= 8 || ((*jitter_count % 120) == 0)) {
        fprintf(stderr,
                "[d_native_processor] video clock source-pts-jitter-ignored source_pts_us=%lld prev_pts_us=%lld raw_interval_s=%.3f used_interval_s=%.3f jitter=%lld\n",
                (long long)in_pts_us,
                (long long)prev_video_pts_us,
                pts_interval_s,
                expected_interval_s,
                jitter_count ? *jitter_count : 0);
    }
}
