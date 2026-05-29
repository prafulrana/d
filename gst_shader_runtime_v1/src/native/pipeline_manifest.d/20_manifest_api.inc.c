int dproc_pipeline_manifest_parse(DProcStage stages[DPROC_MAX_PIPELINE_STAGES],
                                 int* stage_count,
                                 const char* json,
                                 char* err,
                                 size_t err_size) {
    if (!stages || !stage_count) return -1;
    *stage_count = 0;
    if (!json || !*json) {
        snprintf(err, err_size, "pipeline_manifest_json_required");
        return -1;
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    jsmntok_t toks[256];
    int n = jsmn_parse(&parser, json, strlen(json), toks, (unsigned)(sizeof(toks) / sizeof(toks[0])));
    if (n < 1 || toks[0].type != JSMN_ARRAY) {
        snprintf(err, err_size, "pipeline_manifest_json_bad");
        return -1;
    }
    int next = 1;
    for (int i = 0; i < toks[0].size && next < n; i++) {
        int obj = next;
        if (toks[obj].type != JSMN_OBJECT) {
            snprintf(err, err_size, "pipeline_stage_not_object");
            return -1;
        }
        if (*stage_count >= DPROC_MAX_PIPELINE_STAGES) {
            snprintf(err, err_size, "pipeline_stage_overflow");
            return -1;
        }
        DProcStage stage = {0};
        int field = obj + 1;
        for (int p = 0; p < toks[obj].size && field < n; p++) {
            int key = field++;
            if (key >= n || field >= n) {
                snprintf(err, err_size, "pipeline_stage_truncated");
                return -1;
            }
            int val = field;
            if (json_key_eq(json, &toks[key], "id") && toks[val].type == JSMN_STRING) {
                stage.id_hash = fnv1a_span(json + toks[val].start, toks[val].end - toks[val].start);
            } else if (json_key_eq(json, &toks[key], "dims") && toks[val].type == JSMN_OBJECT) {
                parse_dims_object(json, toks, n, val, &stage.in_w, &stage.in_h, &stage.out_w, &stage.out_h);
            }
            field = dproc_json_skip_token(toks, n, val);
        }
        if (!stage.id_hash || stage.in_w <= 0 || stage.in_h <= 0 || stage.out_w <= 0 || stage.out_h <= 0) {
            snprintf(err, err_size, "pipeline_stage_invalid");
            return -1;
        }
        stages[(*stage_count)++] = stage;
        next = dproc_json_skip_token(toks, n, obj);
    }
    if (*stage_count <= 0) {
        snprintf(err, err_size, "pipeline_manifest_empty");
        return -1;
    }
    return 0;
}

static int dproc_model_stage_generates_frames(const DProcModelStage* stage) {
    return stage &&
           stage->kind_code == DPROC_MODEL_KIND_MOTION &&
           stage->output_fps > stage->input_fps;
}

int dproc_model_pipeline_parse(DProcModelStage stages[DPROC_MAX_MODEL_STAGES],
                               int* stage_count,
                               const char* json,
                               char* err,
                               size_t err_size) {
    if (!stages || !stage_count) return -1;
    *stage_count = 0;
    if (!json || !*json) {
        snprintf(err, err_size, "d_pipeline_json_required");
        return -1;
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    jsmntok_t toks[4096];
    int n = jsmn_parse(&parser, json, strlen(json), toks, (unsigned)(sizeof(toks) / sizeof(toks[0])));
    if (n < 1 || toks[0].type != JSMN_OBJECT) {
        snprintf(err, err_size, "d_pipeline_json_bad");
        return -1;
    }

    int stages_idx = -1;
    int clock_policy_seen = 0;
    int encoder_policy_seen = 0;
    int device_policy_seen = 0;
    int caps_seen = 0;
    int timing_seen = 0;
    int filters_seen = 0;
    int units_seen = 0;
    int links_seen = 0;
    int link_plan_seen = 0;
    int next = 1;
    for (int i = 0; i < toks[0].size && next < n; i++) {
        int key = next++;
        if (key >= n || next >= n) {
            snprintf(err, err_size, "d_pipeline_truncated");
            return -1;
        }
        int val = next;
        if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_CLOCKPOLICY) && toks[val].type == JSMN_OBJECT) {
            clock_policy_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_ENCODERPOLICY) && toks[val].type == JSMN_OBJECT) {
            encoder_policy_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_DEVICEPOLICY) && toks[val].type == JSMN_OBJECT) {
            device_policy_seen = 1;
            if (parse_device_policy(json, toks, n, val, err, err_size) < 0) return -1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_CAPS) && toks[val].type == JSMN_OBJECT) {
            caps_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_TIMING) && toks[val].type == JSMN_OBJECT) {
            timing_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_FILTERS) && toks[val].type == JSMN_ARRAY) {
            filters_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_UNITS) && toks[val].type == JSMN_ARRAY) {
            units_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_LINKS) && toks[val].type == JSMN_ARRAY) {
            links_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_LINKPLAN) && toks[val].type == JSMN_OBJECT) {
            link_plan_seen = 1;
        } else if (json_key_eq(json, &toks[key], DPIPELINE_REQUIRED_KEY_STAGES) && toks[val].type == JSMN_ARRAY) {
            stages_idx = val;
        }
        next = dproc_json_skip_token(toks, n, val);
    }
    if (!clock_policy_seen) {
        snprintf(err, err_size, "d_pipeline_clock_policy_required");
        return -1;
    }
    if (!encoder_policy_seen || !device_policy_seen || !caps_seen || !timing_seen ||
        !units_seen ||
        !filters_seen || !links_seen || !link_plan_seen) {
        snprintf(err, err_size, "d_pipeline_required_sections_missing");
        return -1;
    }
    if (stages_idx < 0) {
        snprintf(err, err_size, "d_pipeline_stages_required");
        return -1;
    }

    next = stages_idx + 1;
    int output_clock_owners = 0;
    int frame_generation_owners = 0;
    for (int i = 0; i < toks[stages_idx].size && next < n; i++) {
        if (*stage_count >= DPROC_MAX_MODEL_STAGES) {
            snprintf(err, err_size, "d_pipeline_stage_overflow");
            return -1;
        }
        DProcModelStage stage;
        if (parse_d_model_stage(json, toks, n, next, &stage, err, err_size) < 0) return -1;
        if (stage.output_clock_owner) output_clock_owners++;
        if (dproc_model_stage_generates_frames(&stage)) {
            frame_generation_owners++;
            if (DPIPELINE_FRAME_GENERATION_MUST_OWN_OUTPUT_CLOCK && !stage.output_clock_owner) {
                snprintf(err, err_size, "d_pipeline_frame_generation_must_own_output_clock id=%s", stage.id);
                return -1;
            }
        }
        stages[(*stage_count)++] = stage;
        next = dproc_json_skip_token(toks, n, next);
    }
    if (*stage_count <= 0) {
        snprintf(err, err_size, "d_pipeline_stage_empty");
        return -1;
    }
    if (output_clock_owners != DPIPELINE_REQUIRED_OUTPUT_CLOCK_OWNERS) {
        snprintf(err, err_size, output_clock_owners > 1
                 ? "d_pipeline_multiple_output_clock_owners"
                 : "d_pipeline_output_clock_owner_required");
        return -1;
    }
    if (frame_generation_owners > DPIPELINE_MAX_FRAME_GENERATION_OWNERS) {
        snprintf(err, err_size, "d_pipeline_multiple_frame_generation_owners");
        return -1;
    }
    return 0;
}

int dproc_stage_contains(const DProcStage* stages, int stage_count, uint32_t id) {
    for (int i = 0; stages && i < stage_count; i++) {
        if (stages[i].id_hash == id) return 1;
    }
    return 0;
}

int dproc_stage_dims_for(const DProcStage* stages, int stage_count, uint32_t id,
                        int* w_out, int* h_out) {
    if (!w_out || !h_out) return -1;
    for (int i = 0; stages && i < stage_count; i++) {
        if (stages[i].id_hash == id) {
            *w_out = stages[i].in_w;
            *h_out = stages[i].in_h;
            return 0;
        }
    }
    return -1;
}
