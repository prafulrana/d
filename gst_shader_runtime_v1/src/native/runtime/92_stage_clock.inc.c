static double d_pipeline_stage_sample_clock_s(int stage_clock_enabled,
                                              double stage_clock_fps,
                                              double raw_src_fps,
                                              long long source_seq,
                                              long long accepted_stage_seq,
                                              int n_in) {
    const double fallback_fps = stage_clock_enabled && stage_clock_fps > 0.0 ? stage_clock_fps : 24.0;
    if (stage_clock_enabled && stage_clock_fps > 0.0) {
        return (double)accepted_stage_seq / fallback_fps;
    }
    if (raw_src_fps > 0.0) {
        return (double)source_seq / raw_src_fps;
    }
    return (double)n_in / fallback_fps;
}
