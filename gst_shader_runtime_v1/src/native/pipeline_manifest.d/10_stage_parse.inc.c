static int parse_dims_object(const char* js, const jsmntok_t* toks, int n, int idx,
                             int* in_w, int* in_h, int* out_w, int* out_h) {
    if (!js || !toks || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return -1;
    int w = 0, h = 0;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        if (json_key_eq(js, &toks[key], "w")) {
            json_token_int(js, &toks[val], &w);
        } else if (json_key_eq(js, &toks[key], "h")) {
            json_token_int(js, &toks[val], &h);
        } else if (json_key_eq(js, &toks[key], "in") && toks[val].type == JSMN_OBJECT) {
            parse_dims_object(js, toks, n, val, in_w, in_h, in_w, in_h);
        } else if (json_key_eq(js, &toks[key], "out") && toks[val].type == JSMN_OBJECT) {
            parse_dims_object(js, toks, n, val, out_w, out_h, out_w, out_h);
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    if (w > 0 && h > 0) {
        *in_w = w; *in_h = h; *out_w = w; *out_h = h;
    }
    return 0;
}

typedef struct DProcTimingDraft {
    int input_fps;
    int infer_fps;
    int output_fps;
    int timing_fps;
} DProcTimingDraft;

static int parse_frame_contract(const char* js, const jsmntok_t* toks, int n, int idx,
                                int* w, int* h, int* fps, char* err, size_t err_size) {
    if (!js || !toks || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return -1;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        char key_name[64];
        json_token_key_copy(js, &toks[key], key_name);
        if (!DPIPELINE_FRAME_KEY_IS_ALLOWED(key_name)) {
            snprintf(err, err_size, "d_pipeline_frame_key_unknown=%s", key_name);
            return -1;
        }
        if ((json_key_eq(js, &toks[key], "width") || json_key_eq(js, &toks[key], "w")) && w) {
            json_token_int(js, &toks[val], w);
        } else if ((json_key_eq(js, &toks[key], "height") || json_key_eq(js, &toks[key], "h")) && h) {
            json_token_int(js, &toks[val], h);
        } else if (json_key_eq(js, &toks[key], "fps") && fps) {
            json_token_int(js, &toks[val], fps);
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    return 0;
}

static int parse_model_object(const char* js, const jsmntok_t* toks, int n, int idx,
                              DProcModelStage* stage, int* model_infer_fps,
                              char* err, size_t err_size) {
    if (!js || !toks || !stage || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return -1;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        char key_name[64];
        json_token_key_copy(js, &toks[key], key_name);
        if (!DPIPELINE_MODEL_KEY_IS_ALLOWED(key_name)) {
            snprintf(err, err_size, "d_pipeline_model_key_unknown=%s", key_name);
            return -1;
        }
        if ((json_key_eq(js, &toks[key], "family") || json_key_eq(js, &toks[key], "id")) &&
            toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->family, sizeof(stage->family));
            ascii_lower_inplace(stage->family);
            stage->family_code = model_family_code(stage->family);
        } else if (json_key_eq(js, &toks[key], "inferFps")) {
            int parsed = 0;
            json_token_int(js, &toks[val], &parsed);
            if (model_infer_fps) *model_infer_fps = parsed;
        } else if (json_key_eq(js, &toks[key], "path") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->engine_path, sizeof(stage->engine_path));
        } else if (json_key_eq(js, &toks[key], "passThrough") ||
                   json_key_eq(js, &toks[key], "passthrough")) {
            json_token_bool(js, &toks[val], &stage->pass_through);
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    return 0;
}

static int parse_timing_object(const char* js, const jsmntok_t* toks, int n, int idx,
                               DProcModelStage* stage, DProcTimingDraft* timing,
                               char* err, size_t err_size) {
    if (!js || !toks || !stage || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return -1;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        char key_name[64];
        json_token_key_copy(js, &toks[key], key_name);
        if (!DPIPELINE_TIMING_KEY_IS_ALLOWED(key_name)) {
            snprintf(err, err_size, "d_pipeline_timing_key_unknown=%s", key_name);
            return -1;
        }
        if ((json_key_eq(js, &toks[key], "cadenceOwner") || json_key_eq(js, &toks[key], "role")) &&
            toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->timing_role, sizeof(stage->timing_role));
            ascii_lower_inplace(stage->timing_role);
        } else if ((json_key_eq(js, &toks[key], "ptsPolicy") || json_key_eq(js, &toks[key], "pts")) &&
                   toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->pts_policy, sizeof(stage->pts_policy));
            ascii_lower_inplace(stage->pts_policy);
        } else if (json_key_eq(js, &toks[key], "frameHoldPolicy") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->frame_hold_policy, sizeof(stage->frame_hold_policy));
            ascii_lower_inplace(stage->frame_hold_policy);
        } else if (json_key_eq(js, &toks[key], "inputFps")) {
            json_token_int(js, &toks[val], &timing->input_fps);
        } else if (json_key_eq(js, &toks[key], "inferFps")) {
            json_token_int(js, &toks[val], &timing->infer_fps);
        } else if (json_key_eq(js, &toks[key], "outputFps") ||
                   json_key_eq(js, &toks[key], "fps")) {
            if (json_key_eq(js, &toks[key], "outputFps")) {
                json_token_int(js, &toks[val], &timing->output_fps);
            } else {
                json_token_int(js, &toks[val], &timing->timing_fps);
            }
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    return 0;
}

static int parse_stage_params_object(const char* js, const jsmntok_t* toks, int n, int idx,
                                     DProcModelStage* stage, char* err, size_t err_size) {
    if (!js || !toks || !stage || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return -1;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        double dv = 0.0;
        char key_name[64];
        json_token_key_copy(js, &toks[key], key_name);
        if (!DPIPELINE_PARAMS_KEY_IS_ALLOWED(key_name)) {
            snprintf(err, err_size, "d_pipeline_params_key_unknown=%s", key_name);
            return -1;
        }
        if ((json_key_eq(js, &toks[key], "nvofStrength") ||
             json_key_eq(js, &toks[key], "genStrength") ||
             json_key_eq(js, &toks[key], "strength") ||
             json_key_eq(js, &toks[key], "motionStrength")) &&
            json_token_double(js, &toks[val], &dv)) {
            stage->motion_strength = (float)dv;
        } else if (json_key_eq(js, &toks[key], "motionGuide") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->motion_guide, sizeof(stage->motion_guide));
            ascii_lower_inplace(stage->motion_guide);
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    return 0;
}

static int parse_device_policy(const char* js, const jsmntok_t* toks, int n, int idx,
                               char* err, size_t err_size) {
    if (!js || !toks || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) return 0;
    int next = idx + 1;
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) return -1;
        int val = next;
        if (json_key_eq(js, &toks[key], "cpuFallback") && toks[val].type == JSMN_STRING &&
            !token_string_equals(js, &toks[val], DPIPELINE_DEVICE_CPU_FALLBACK)) {
            snprintf(err, err_size, "d_pipeline_cpu_fallback_forbidden");
            return -1;
        }
        if (json_key_eq(js, &toks[key], "residency") && toks[val].type == JSMN_STRING &&
            !token_string_equals(js, &toks[val], DPIPELINE_DEVICE_RESIDENCY)) {
            snprintf(err, err_size, "d_pipeline_non_gpu_residency_forbidden");
            return -1;
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    return 0;
}

static int parse_d_model_stage(const char* js, const jsmntok_t* toks, int n, int idx,
                               DProcModelStage* stage, char* err, size_t err_size) {
    if (!js || !toks || !stage || idx < 0 || idx >= n || toks[idx].type != JSMN_OBJECT) {
        snprintf(err, err_size, "d_pipeline_stage_not_object");
        return -1;
    }
    memset(stage, 0, sizeof(*stage));
    int next = idx + 1;
    char device[32] = "gpu";
    int model_infer_fps = 0;
    DProcTimingDraft timing = {0};
    for (int i = 0; i < toks[idx].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) {
            snprintf(err, err_size, "d_pipeline_stage_truncated");
            return -1;
        }
        int val = next;
        char key_name[64];
        json_token_key_copy(js, &toks[key], key_name);
        if (!DPIPELINE_STAGE_KEY_IS_ALLOWED(key_name)) {
            snprintf(err, err_size, "d_pipeline_stage_key_unknown=%s", key_name);
            return -1;
        }
        if (json_key_eq(js, &toks[key], "id") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->id, sizeof(stage->id));
        } else if (json_key_eq(js, &toks[key], "label") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->label, sizeof(stage->label));
        } else if (json_key_eq(js, &toks[key], "kind") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->kind, sizeof(stage->kind));
            ascii_lower_inplace(stage->kind);
        } else if (json_key_eq(js, &toks[key], "engine") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->engine, sizeof(stage->engine));
            ascii_lower_inplace(stage->engine);
        } else if (json_key_eq(js, &toks[key], "op") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], stage->op, sizeof(stage->op));
            ascii_lower_inplace(stage->op);
        } else if (json_key_eq(js, &toks[key], "device") && toks[val].type == JSMN_STRING) {
            json_token_copy(js, &toks[val], device, sizeof(device));
            ascii_lower_inplace(device);
        } else if (json_key_eq(js, &toks[key], "input") && toks[val].type == JSMN_OBJECT) {
            if (parse_frame_contract(js, toks, n, val,
                                     &stage->input_w, &stage->input_h, &stage->input_fps,
                                     err, err_size) < 0) return -1;
        } else if (json_key_eq(js, &toks[key], "output") && toks[val].type == JSMN_OBJECT) {
            if (parse_frame_contract(js, toks, n, val,
                                     &stage->output_w, &stage->output_h, &stage->output_fps,
                                     err, err_size) < 0) return -1;
        } else if (json_key_eq(js, &toks[key], "model")) {
            if (toks[val].type == JSMN_OBJECT) {
                if (parse_model_object(js, toks, n, val, stage, &model_infer_fps, err, err_size) < 0) return -1;
            } else if (toks[val].type == JSMN_STRING) {
                json_token_copy(js, &toks[val], stage->family, sizeof(stage->family));
                ascii_lower_inplace(stage->family);
                stage->family_code = model_family_code(stage->family);
            }
        } else if (json_key_eq(js, &toks[key], "timing") && toks[val].type == JSMN_OBJECT) {
            if (parse_timing_object(js, toks, n, val, stage, &timing, err, err_size) < 0) return -1;
        } else if (json_key_eq(js, &toks[key], "params") && toks[val].type == JSMN_OBJECT) {
            if (parse_stage_params_object(js, toks, n, val, stage, err, err_size) < 0) return -1;
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    if (strcmp(stage->op, "infer-pass") == 0 || strcmp(stage->op, "trt-pass") == 0 ||
        strcmp(stage->op, "passthrough") == 0) {
        stage->pass_through = 1;
    }
    if (strcmp(device, "gpu") != 0) {
        snprintf(err, err_size, "d_pipeline_cpu_or_host_stage_forbidden id=%s", stage->id);
        return -1;
    }
    if (!stage->id[0]) snprintf(stage->id, sizeof(stage->id), "stage_%d", idx);
    if (!stage->label[0]) snprintf(stage->label, sizeof(stage->label), "%s", stage->id);
    stage->kind_code = model_kind_code(stage->kind, stage->op);
    if (stage->kind_code == DPROC_MODEL_KIND_UNKNOWN && stage->family_code != DPROC_MODEL_FAMILY_NONE) {
        stage->kind_code = DPROC_MODEL_KIND_MODEL;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL && stage->family_code == DPROC_MODEL_FAMILY_NONE) {
        stage->family_code = model_family_code(stage->engine);
        if (stage->family_code != DPROC_MODEL_FAMILY_NONE) {
            snprintf(stage->family, sizeof(stage->family), "%s", dproc_model_family_name(stage->family_code));
        }
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL && model_infer_fps > 0) {
        if (timing.infer_fps > 0 && timing.infer_fps != model_infer_fps) {
            snprintf(err, err_size, "d_pipeline_model_timing_infer_fps_mismatch id=%s model=%d timing=%d",
                     stage->id, model_infer_fps, timing.infer_fps);
            return -1;
        }
        stage->infer_fps = model_infer_fps;
    } else if (timing.infer_fps > 0) {
        stage->infer_fps = timing.infer_fps;
    }
    if (timing.input_fps > 0 && stage->input_fps > 0 && timing.input_fps != stage->input_fps) {
        snprintf(err, err_size, "d_pipeline_timing_input_fps_mismatch id=%s input=%d timing=%d",
                 stage->id, stage->input_fps, timing.input_fps);
        return -1;
    }
    if (timing.output_fps > 0 && stage->output_fps > 0 && timing.output_fps != stage->output_fps) {
        snprintf(err, err_size, "d_pipeline_timing_output_fps_mismatch id=%s output=%d timing=%d",
                 stage->id, stage->output_fps, timing.output_fps);
        return -1;
    }
    if (timing.timing_fps > 0 && stage->output_fps > 0 && timing.timing_fps != stage->output_fps) {
        snprintf(err, err_size, "d_pipeline_timing_fps_mismatch id=%s output=%d timing=%d",
                 stage->id, stage->output_fps, timing.timing_fps);
        return -1;
    }
    if (timing.input_fps > 0) stage->input_fps = timing.input_fps;
    if (timing.output_fps > 0) stage->output_fps = timing.output_fps;
    if (timing.timing_fps > 0) stage->timing_fps = timing.timing_fps;
    stage->engine_code = model_engine_code(stage->engine, stage->family_code);
    if (stage->kind_code == DPROC_MODEL_KIND_MOTION && stage->engine_code == DPROC_MODEL_ENGINE_UNKNOWN) {
        stage->engine_code = DPROC_MODEL_ENGINE_CUDA;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_CUDA && stage->engine_code == DPROC_MODEL_ENGINE_UNKNOWN) {
        stage->engine_code = DPROC_MODEL_ENGINE_CUDA;
    }
    if (stage->input_w <= 0 || stage->input_h <= 0 || stage->output_w <= 0 || stage->output_h <= 0) {
        snprintf(err, err_size, "d_pipeline_stage_bad_caps id=%s", stage->id);
        return -1;
    }
    if (stage->input_fps <= 0) stage->input_fps = DPIPELINE_FPS_DEFAULT;
    if (stage->output_fps <= 0) stage->output_fps = stage->input_fps;
    if (stage->infer_fps <= 0) stage->infer_fps = stage->output_fps;
    if (stage->timing_fps <= 0) stage->timing_fps = stage->output_fps;
    if (stage->input_fps < DPIPELINE_FPS_MIN || stage->input_fps > DPIPELINE_FPS_MAX ||
        stage->output_fps < DPIPELINE_FPS_MIN || stage->output_fps > DPIPELINE_FPS_MAX ||
        stage->infer_fps < DPIPELINE_FPS_MIN || stage->infer_fps > DPIPELINE_FPS_MAX ||
        stage->timing_fps < DPIPELINE_FPS_MIN || stage->timing_fps > DPIPELINE_FPS_MAX) {
        snprintf(err, err_size, "d_pipeline_stage_bad_fps id=%s fps=%d/%d/%d/%d",
                 stage->id, stage->input_fps, stage->infer_fps,
                 stage->output_fps, stage->timing_fps);
        return -1;
    }
    if (!isfinite(stage->motion_strength) || stage->motion_strength < 0.0f) stage->motion_strength = 0.0f;
    if (stage->motion_strength > 1.20f) stage->motion_strength = 1.20f;
    if (!stage->timing_role[0]) {
        if (stage->kind_code == DPROC_MODEL_KIND_MOTION) {
            snprintf(stage->timing_role, sizeof(stage->timing_role), "%s", DPIPELINE_TIMING_ROLE_CADENCE_ADAPTER);
        } else if (stage->kind_code == DPROC_MODEL_KIND_MODEL && stage->pass_through) {
            snprintf(stage->timing_role, sizeof(stage->timing_role), "%s", DPIPELINE_TIMING_ROLE_PASS_CADENCE);
        } else if (stage->kind_code == DPROC_MODEL_KIND_MODEL && stage->output_h == 2160) {
            snprintf(stage->timing_role, sizeof(stage->timing_role), "%s", DPIPELINE_TIMING_ROLE_OUTPUT_CLOCK_OWNER);
        } else if (stage->kind_code == DPROC_MODEL_KIND_MODEL) {
            snprintf(stage->timing_role, sizeof(stage->timing_role), "%s", DPIPELINE_TIMING_ROLE_GATE_INFERENCE);
        } else {
            snprintf(stage->timing_role, sizeof(stage->timing_role), "%s", DPIPELINE_TIMING_ROLE_PASS_CADENCE);
        }
    }
    if (!stage->pts_policy[0]) {
        snprintf(stage->pts_policy, sizeof(stage->pts_policy), "%s",
                 DPIPELINE_TIMING_ROLE_DEFAULT_PTS(stage->timing_role));
    }
    if (!stage->frame_hold_policy[0]) {
        snprintf(stage->frame_hold_policy, sizeof(stage->frame_hold_policy), "%s",
                 DPIPELINE_TIMING_ROLE_DEFAULT_FRAME_HOLD(stage->timing_role));
    }
    if (!stage->motion_guide[0]) {
        snprintf(stage->motion_guide, sizeof(stage->motion_guide), "%s", "output");
    }
    if (!DPIPELINE_TIMING_ROLE_IS_VALID(stage->timing_role)) {
        snprintf(err, err_size, "d_pipeline_bad_timing_role id=%s role=%s", stage->id, stage->timing_role);
        return -1;
    }
    if (!DPIPELINE_PTS_POLICY_IS_VALID(stage->pts_policy)) {
        snprintf(err, err_size, "d_pipeline_bad_pts_policy id=%s pts=%s", stage->id, stage->pts_policy);
        return -1;
    }
    if (!DPIPELINE_FRAME_HOLD_POLICY_IS_VALID(stage->frame_hold_policy)) {
        snprintf(err, err_size, "d_pipeline_bad_frame_hold_policy id=%s frameHold=%s",
                 stage->id, stage->frame_hold_policy);
        return -1;
    }
    stage->timing_owner = strcmp(stage->timing_role, DPIPELINE_TIMING_ROLE_PASS_CADENCE) != 0;
    stage->output_clock_owner = strcmp(stage->timing_role, DPIPELINE_TIMING_ROLE_OUTPUT_CLOCK_OWNER) == 0;
    if (stage->pass_through && stage->engine_code != DPROC_MODEL_ENGINE_TRT) {
        snprintf(err, err_size, "d_pipeline_pass_through_requires_trt id=%s engine=%s",
                 stage->id, stage->engine);
        return -1;
    }
    if (stage->pass_through &&
        (stage->input_w != stage->output_w || stage->input_h != stage->output_h ||
         stage->input_fps != stage->output_fps)) {
        snprintf(err, err_size, "d_pipeline_pass_through_caps_mismatch id=%s", stage->id);
        return -1;
    }
    if (stage->kind_code == DPROC_MODEL_KIND_MODEL &&
        !stage->pass_through &&
        (stage->family_code == DPROC_MODEL_FAMILY_NONE ||
         stage->engine_code == DPROC_MODEL_ENGINE_UNKNOWN)) {
        snprintf(err, err_size, "d_pipeline_model_stage_unknown_engine id=%s family=%s engine=%s",
                 stage->id, stage->family, stage->engine);
        return -1;
    }
    return 0;
}
