static double positive_min_fps(double current, int value) {
    if (value <= 0) return current;
    const double fps = (double)value;
    return current <= 0.0 || fps < current ? fps : current;
}

static double d_pipeline_source_gate_fps(const BakeWorker* w, double src_fps,
                                         int* stage_index_out,
                                         int* output_clock_index_out,
                                         const char** policy_out) {
    if (stage_index_out) *stage_index_out = -1;
    if (output_clock_index_out) *output_clock_index_out = -1;
    if (policy_out) *policy_out = "graph-stage-fps";
    if (!w || w->model_stage_count <= 0 || src_fps <= 0.0) return 0.0;

    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (s->output_clock_owner && output_clock_index_out) *output_clock_index_out = i;
    }

    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        double gate_fps = 0.0;
        if (s->kind_code == DPROC_MODEL_KIND_MOTION) {
            gate_fps = positive_min_fps(gate_fps, s->input_fps);
            gate_fps = positive_min_fps(gate_fps, s->infer_fps);
            gate_fps = positive_min_fps(gate_fps, s->output_fps);
            gate_fps = positive_min_fps(gate_fps, s->timing_fps);
        } else if (s->kind_code == DPROC_MODEL_KIND_MODEL) {
            gate_fps = positive_min_fps(gate_fps, s->input_fps);
            gate_fps = positive_min_fps(gate_fps, s->infer_fps);
            gate_fps = positive_min_fps(gate_fps, s->output_fps);
            gate_fps = positive_min_fps(gate_fps, s->timing_fps);
        }

        if (gate_fps > 0.0 && gate_fps < src_fps - 0.25) {
            if (stage_index_out) *stage_index_out = i;
            if (policy_out) *policy_out = s->timing_role[0] ? s->timing_role : "graph-stage-fps";
            return gate_fps;
        }
    }
    return 0.0;
}

static int d_pipeline_first_scale_model_index(const BakeWorker* w) {
    if (!w) return -1;
    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (s->kind_code == DPROC_MODEL_KIND_MODEL && !s->pass_through) return i;
    }
    return -1;
}

static int d_pipeline_last_scale_model_index(const BakeWorker* w) {
    int idx = -1;
    if (!w) return idx;
    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (s->kind_code == DPROC_MODEL_KIND_MODEL && !s->pass_through) idx = i;
    }
    return idx;
}

static int d_pipeline_motion_increases_fps(const DProcModelStage* s) {
    return s && s->kind_code == DPROC_MODEL_KIND_MOTION && s->output_fps > s->input_fps;
}

static int d_pipeline_frame_generation_owner_index(const BakeWorker* w) {
    if (!w) return -1;
    int idx = -1;
    for (int i = 0; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (!d_pipeline_motion_increases_fps(s)) continue;
        if (s->output_clock_owner ||
            strcmp(s->timing_role, DPIPELINE_TIMING_ROLE_OUTPUT_CLOCK_OWNER) == 0) {
            return i;
        }
        if (idx < 0) idx = i;
    }
    return idx;
}

static int d_pipeline_source_motion_index(const BakeWorker* w) {
    if (!w) return -1;
    const int first_model = d_pipeline_first_scale_model_index(w);
    for (int i = 0; i < w->model_stage_count; i++) {
        if (first_model >= 0 && i >= first_model) break;
        const DProcModelStage* s = &w->model_stages[i];
        if (!d_pipeline_motion_increases_fps(s)) continue;
        if (first_model >= 0) {
            const DProcModelStage* m = &w->model_stages[first_model];
            if (s->output_w != m->input_w || s->output_h != m->input_h) continue;
        }
        return i;
    }
    return -1;
}

static int d_pipeline_final_motion_index(const BakeWorker* w) {
    if (!w) return -1;
    const int last_model = d_pipeline_last_scale_model_index(w);
    for (int i = 0; i < w->model_stage_count; i++) {
        if (last_model >= 0 && i <= last_model) continue;
        const DProcModelStage* s = &w->model_stages[i];
        if (!d_pipeline_motion_increases_fps(s)) continue;
        if (s->output_clock_owner ||
            strcmp(s->timing_role, DPIPELINE_TIMING_ROLE_OUTPUT_CLOCK_OWNER) == 0 ||
            strcmp(s->timing_role, DPIPELINE_TIMING_ROLE_CADENCE_ADAPTER) == 0) {
            return i;
        }
    }
    return -1;
}

static int d_pipeline_next_scale_model_after(const BakeWorker* w, int idx) {
    if (!w) return -1;
    for (int i = idx + 1; i < w->model_stage_count; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (s->kind_code == DPROC_MODEL_KIND_MODEL && !s->pass_through) return i;
    }
    return -1;
}

static int d_pipeline_intermediate_motion_index(const BakeWorker* w) {
    if (!w) return -1;
    const int first_model = d_pipeline_first_scale_model_index(w);
    const int last_model = d_pipeline_last_scale_model_index(w);
    if (first_model < 0 || last_model <= first_model) return -1;
    for (int i = first_model + 1; i < last_model; i++) {
        const DProcModelStage* s = &w->model_stages[i];
        if (d_pipeline_motion_increases_fps(s) &&
            strcmp(s->timing_role, DPIPELINE_TIMING_ROLE_CADENCE_ADAPTER) == 0) {
            return i;
        }
    }
    return -1;
}

static float d_pipeline_motion_strength(const BakeWorker* w, int idx) {
    if (!w || idx < 0 || idx >= w->model_stage_count) return 0.0f;
    const float strength = w->model_stages[idx].motion_strength;
    if (!isfinite(strength) || strength < 0.0f) return 0.0f;
    return strength > 1.20f ? 1.20f : strength;
}
