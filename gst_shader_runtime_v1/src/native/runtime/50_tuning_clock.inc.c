static int env_int_clamped(const char* name, int def, int min, int max) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return def;
    int value = atoi(raw);
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static double env_double_clamped(const char* name, double def, double min, double max) {
    const char* raw = getenv(name);
    if (!raw || !*raw) return def;
    char* end = NULL;
    double value = strtod(raw, &end);
    if (end == raw || !isfinite(value)) return def;
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static float clamp_float(float value, float min, float max) {
    if (!isfinite(value)) return min;
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int json_key_eq(const char* js, const jsmntok_t* tok, const char* key) {
    if (!js || !tok || tok->type != JSMN_STRING || !key) return 0;
    int len = tok->end - tok->start;
    return (int)strlen(key) == len && strncmp(js + tok->start, key, len) == 0;
}

static int json_token_double(const char* js, const jsmntok_t* tok, double* out) {
    if (!js || !tok || !out || tok->start < 0 || tok->end <= tok->start) return 0;
    int len = tok->end - tok->start;
    if (len <= 0 || len >= 64) return 0;
    char tmp[64];
    memcpy(tmp, js + tok->start, (size_t)len);
    tmp[len] = 0;
    char* end = NULL;
    double value = strtod(tmp, &end);
    if (!end || end == tmp || !isfinite(value)) return 0;
    *out = value;
    return 1;
}

static int json_token_int(const char* js, const jsmntok_t* tok, int* out) {
    double value;
    if (!json_token_double(js, tok, &value)) return 0;
    *out = (int)llround(value);
    return 1;
}

static int json_token_audio_superres_mode(const char* js, const jsmntok_t* tok, int* out) {
    if (!js || !tok || !out || tok->start < 0 || tok->end <= tok->start) return 0;
    int len = tok->end - tok->start;
    if (len <= 0 || len >= 32) return 0;
    char tmp[32];
    for (int i = 0; i < len; i++) {
        tmp[i] = (char)tolower((unsigned char)js[tok->start + i]);
    }
    tmp[len] = 0;
    if (strcmp(tmp, "force") == 0 || strcmp(tmp, "forced") == 0 || strcmp(tmp, "1") == 0) {
        *out = DPROC_AUDIO_SR_FORCE;
        return 1;
    }
    if (strcmp(tmp, "off") == 0 || strcmp(tmp, "none") == 0 || strcmp(tmp, "2") == 0) {
        *out = DPROC_AUDIO_SR_OFF;
        return 1;
    }
    if (strcmp(tmp, "auto") == 0 || strcmp(tmp, "0") == 0) {
        *out = DPROC_AUDIO_SR_AUTO;
        return 1;
    }
    return 0;
}

static const char* audio_superres_mode_name(int mode) {
    switch (mode) {
        case DPROC_AUDIO_SR_FORCE: return "force";
        case DPROC_AUDIO_SR_OFF: return "off";
        case DPROC_AUDIO_SR_AUTO:
        default: return "auto";
    }
}

static int should_use_maxine_audio(const BakeCtx* c, const BakeRequest* req) {
    if (!c || !c->audio_dec_ctx || !req || req->audio_passthrough) return 0;
    if (req->audio_superres_mode == DPROC_AUDIO_SR_OFF) return 0;
    if (req->audio_superres_mode == DPROC_AUDIO_SR_FORCE) return 1;
    const int sr = c->audio_dec_ctx->sample_rate;
    const int ch = c->audio_dec_ctx->ch_layout.nb_channels;
    return sr > 0 && sr <= 16000 && ch <= 2;
}

static void clamp_runtime_tuning(BakeRequest* req) {
    if (!req) return;
    req->contrast = clamp_float(req->contrast > 0.0f ? req->contrast : 1.06f, 0.80f, 1.30f);
    req->saturation = clamp_float(req->saturation > 0.0f ? req->saturation : 1.07f, 0.70f, 1.35f);
    req->gamma = clamp_float(req->gamma > 0.0f ? req->gamma : 0.98f, 0.80f, 1.20f);
    req->cas_strength = clamp_float(req->cas_strength, 0.0f, 1.20f);
    req->contrast_boost = clamp_float(req->contrast_boost, 0.0f, 1.20f);
    req->grain_strength = clamp_float(req->grain_strength, 0.0f, 0.04f);
    req->temporal_strength = clamp_float(req->temporal_strength, 0.0f, 1.20f);
    req->edge_stability = clamp_float(req->edge_stability, 0.0f, 1.20f);
    req->custom_shader_intensity = clamp_float(req->custom_shader_intensity, 0.0f, 1.0f);
    req->audio_cleanup_strength = clamp_float(req->audio_cleanup_strength, 0.0f, 1.0f);
    if (req->audio_superres_mode < DPROC_AUDIO_SR_AUTO || req->audio_superres_mode > DPROC_AUDIO_SR_OFF) {
        req->audio_superres_mode = DPROC_AUDIO_SR_AUTO;
    }
    req->audio_passthrough = req->audio_passthrough ? 1 : 0;
    if (req->audio_eq_mode < 0) req->audio_eq_mode = 0;
    if (req->audio_eq_mode > 5) req->audio_eq_mode = 5;
    if (req->audio_delay_ms < -1000) req->audio_delay_ms = -1000;
    if (req->audio_delay_ms > 1000) req->audio_delay_ms = 1000;
}

static void apply_audio_delay(BakeCtx* c, int audio_delay_ms) {
    if (!c) return;
    if (audio_delay_ms < -1000) audio_delay_ms = -1000;
    if (audio_delay_ms > 1000) audio_delay_ms = 1000;
    c->audio_delay_samples = (int64_t)llround((double)audio_delay_ms * 48.0);
}

static int64_t audio_target_samples_for_pts(BakeCtx* c, int64_t pts_us) {
    if (!c || pts_us == AV_NOPTS_VALUE || pts_us < c->audio_source_base_pts_us) return 0;
    const int64_t rel_us = pts_us - c->audio_source_base_pts_us;
    const int sample_rate = c->audio_enc_ctx && c->audio_enc_ctx->sample_rate > 0
        ? c->audio_enc_ctx->sample_rate
        : 48000;
    return av_rescale_q(rel_us, (AVRational){1, 1000000}, (AVRational){1, sample_rate});
}

static int64_t pending_audio_output_samples(BakeCtx* c) {
    if (!c) return 0;
    int64_t pending = c->audio_fifo ? av_audio_fifo_size(c->audio_fifo) : 0;
    if (c->audio_maxine_active) {
        const int afx_pending = c->afx_input_fifo ? av_audio_fifo_size(c->afx_input_fifo) : 0;
        const int bed_pending = c->audio_bed_fifo ? av_audio_fifo_size(c->audio_bed_fifo) : 0;
        const int64_t afx_as_48k = av_rescale_q(afx_pending,
                                                (AVRational){1, DPROC_AFX_INPUT_RATE},
                                                (AVRational){1, DPROC_AFX_OUTPUT_RATE});
        pending += afx_as_48k > (int64_t)bed_pending ? afx_as_48k : (int64_t)bed_pending;
    }
    return pending;
}

static void update_live_audio_encode_limit(BakeCtx* c, double video_clock_s) {
    if (!c || !c->audio_pacing_enabled || !c->audio_enc_ctx || c->audio_enc_ctx->sample_rate <= 0) return;
    if (video_clock_s < 0.0) video_clock_s = 0.0;
    const int64_t video_samples = (int64_t)llround(video_clock_s * (double)c->audio_enc_ctx->sample_rate);
    const int64_t limit = video_samples + c->max_audio_lead_samples;
    if (limit > c->audio_encode_limit_samples) c->audio_encode_limit_samples = limit;
}

static void maybe_anchor_audio_clock(BakeCtx* c, int64_t pts_us, const char* reason) {
    if (!c || !c->audio_enc_ctx || pts_us == AV_NOPTS_VALUE) return;
    if (c->live_clock_mode == 1) {
        if (!c->audio_clock_initialized) {
            c->audio_source_base_pts_us = pts_us;
            c->audio_next_pts = 0;
            c->audio_clock_initialized = 1;
            fprintf(stderr,
                    "[d_native_processor] audio clock sasta-pts mode reason=%s source_pts_us=%lld audio_next_pts=%lld delay_samples=%lld\n",
                    reason ? reason : "audio",
                    (long long)pts_us,
                    (long long)c->audio_next_pts,
                    (long long)c->audio_delay_samples);
            return;
        }
        const int64_t target = audio_target_samples_for_pts(c, pts_us);
        const int64_t pending = pending_audio_output_samples(c);
        const int64_t encoded_tail = c->audio_next_pts + pending;
        const int64_t delta = target - encoded_tail;
        const int sample_rate = c->audio_enc_ctx->sample_rate;
        const int64_t max_live_gap = sample_rate / 2;
        if (delta > max_live_gap) {
            const int64_t old_base = c->audio_source_base_pts_us;
            const int64_t encoded_tail_us = av_rescale_q(encoded_tail,
                                                         (AVRational){1, sample_rate},
                                                         (AVRational){1, 1000000});
            c->audio_clock_resyncs++;
            c->audio_source_base_pts_us = pts_us - encoded_tail_us;
            fprintf(stderr,
                    "[d_native_processor] audio clock sasta-pts-gap-squashed reason=%s source_pts_us=%lld old_base=%lld new_base=%lld next=%lld target=%lld pending=%lld delta_samples=%lld resyncs=%lld\n",
                    reason ? reason : "audio",
                    (long long)pts_us,
                    (long long)old_base,
                    (long long)c->audio_source_base_pts_us,
                    (long long)c->audio_next_pts,
                    (long long)target,
                    (long long)pending,
                    (long long)delta,
                    c->audio_clock_resyncs);
            return;
        } else if (delta < -(sample_rate * 2LL)) {
            const int64_t old_base = c->audio_source_base_pts_us;
            const int64_t encoded_tail_us = av_rescale_q(encoded_tail,
                                                         (AVRational){1, sample_rate},
                                                         (AVRational){1, 1000000});
            c->audio_clock_resyncs++;
            c->audio_source_base_pts_us = pts_us - encoded_tail_us;
            fprintf(stderr,
                    "[d_native_processor] audio clock sasta-pts-backward-squashed reason=%s source_pts_us=%lld old_base=%lld new_base=%lld next=%lld target=%lld pending=%lld delta_samples=%lld resyncs=%lld\n",
                    reason ? reason : "audio",
                    (long long)pts_us,
                    (long long)old_base,
                    (long long)c->audio_source_base_pts_us,
                    (long long)c->audio_next_pts,
                    (long long)target,
                    (long long)pending,
                    (long long)delta,
                    c->audio_clock_resyncs);
        }
        return;
    }
    if (!c->audio_clock_initialized) {
        c->audio_source_base_pts_us = pts_us;
        c->audio_next_pts = 0;
        c->audio_clock_initialized = 1;
        fprintf(stderr,
                "[d_native_processor] audio clock anchored reason=%s source_pts_us=%lld base_pts_us=%lld audio_next_pts=%lld delay_samples=%lld\n",
                reason ? reason : "audio",
                (long long)pts_us,
                (long long)c->audio_source_base_pts_us,
                (long long)c->audio_next_pts,
                (long long)c->audio_delay_samples);
        return;
    }
    const int64_t target = audio_target_samples_for_pts(c, pts_us);
    const int queued = c->audio_fifo ? av_audio_fifo_size(c->audio_fifo) : 0;
    const int64_t encoded_tail = c->audio_next_pts + queued;
    const int64_t delta = target - encoded_tail;
    const int64_t threshold = c->audio_enc_ctx->sample_rate * 3LL;
    if (llabs(delta) <= threshold) return;
    c->audio_clock_resyncs++;
    if (queued == 0 && target > c->audio_next_pts) {
        fprintf(stderr,
                "[d_native_processor] audio clock forward-resync reason=%s source_pts_us=%lld old_next=%lld new_next=%lld delta_samples=%lld resyncs=%lld\n",
                reason ? reason : "audio",
                (long long)pts_us,
                (long long)c->audio_next_pts,
                (long long)target,
                (long long)delta,
                c->audio_clock_resyncs);
        c->audio_next_pts = target;
    } else {
        fprintf(stderr,
                "[d_native_processor] audio clock discontinuity kept reason=%s source_pts_us=%lld next=%lld queued=%d target=%lld delta_samples=%lld resyncs=%lld\n",
                reason ? reason : "audio",
                (long long)pts_us,
                (long long)c->audio_next_pts,
                queued,
                (long long)target,
                (long long)delta,
                c->audio_clock_resyncs);
    }
}

static void maybe_reload_live_tuning(BakeCtx* c, BakeRequest* live_req, int force) {
    if (!c || !live_req || !live_req->control_path || !*live_req->control_path) return;
    double now = now_seconds();
    if (!force && c->last_control_check_s > 0.0 && now - c->last_control_check_s < 0.50) return;
    c->last_control_check_s = now;

    struct stat st;
    if (stat(live_req->control_path, &st) != 0) {
        if (force) fprintf(stderr, "[d_native_processor] live tuning control missing path=%s err=%s\n", live_req->control_path, strerror(errno));
        return;
    }
    if (!force && st.st_mtim.tv_sec == c->control_mtime_sec && st.st_mtim.tv_nsec == c->control_mtime_nsec) return;

    FILE* f = fopen(live_req->control_path, "rb");
    if (!f) {
        fprintf(stderr, "[d_native_processor] live tuning open failed path=%s err=%s\n", live_req->control_path, strerror(errno));
        return;
    }
    char json[4096];
    size_t len = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[len] = 0;
    if (len == 0) return;

    jsmn_parser parser;
    jsmn_init(&parser);
    // The resolved pipeline manifest is an array of objects nested inside the
    // request, so this needs headroom beyond simple key/value tuning fields.
    jsmntok_t toks[1024];
    int n = jsmn_parse(&parser, json, len, toks, (unsigned)(sizeof(toks) / sizeof(toks[0])));
    if (n < 1 || toks[0].type != JSMN_OBJECT) {
        fprintf(stderr, "[d_native_processor] live tuning bad_json path=%s\n", live_req->control_path);
        return;
    }

    BakeRequest before = *live_req;
    for (int i = 1; i < n - 1; i++) {
        if (toks[i].type != JSMN_STRING) continue;
        double dv = 0.0;
        int iv = 0;
        if (json_key_eq(json, &toks[i], "contrast") && json_token_double(json, &toks[i+1], &dv)) { live_req->contrast = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "saturation") && json_token_double(json, &toks[i+1], &dv)) { live_req->saturation = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "gamma") && json_token_double(json, &toks[i+1], &dv)) { live_req->gamma = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "cas_strength") && json_token_double(json, &toks[i+1], &dv)) { live_req->cas_strength = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "contrast_boost") && json_token_double(json, &toks[i+1], &dv)) { live_req->contrast_boost = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "grain_strength") && json_token_double(json, &toks[i+1], &dv)) { live_req->grain_strength = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "temporal_strength") && json_token_double(json, &toks[i+1], &dv)) { live_req->temporal_strength = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "edge_stability") && json_token_double(json, &toks[i+1], &dv)) { live_req->edge_stability = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "custom_shader_intensity") && json_token_double(json, &toks[i+1], &dv)) { live_req->custom_shader_intensity = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "audio_cleanup_strength") && json_token_double(json, &toks[i+1], &dv)) { live_req->audio_cleanup_strength = (float)dv; i++; }
        else if (json_key_eq(json, &toks[i], "audio_superres_mode") && json_token_audio_superres_mode(json, &toks[i+1], &iv)) { live_req->audio_superres_mode = iv; i++; }
        else if (json_key_eq(json, &toks[i], "audio_eq_mode") && json_token_int(json, &toks[i+1], &iv)) { live_req->audio_eq_mode = iv; i++; }
        else if (json_key_eq(json, &toks[i], "audio_delay_ms") && json_token_int(json, &toks[i+1], &iv)) { live_req->audio_delay_ms = iv; i++; }
        else { i++; }
    }
    clamp_runtime_tuning(live_req);
    set_maxine_audio_cleanup_strength(c, live_req->audio_cleanup_strength, 0);
    apply_audio_delay(c, live_req->audio_delay_ms);
    if (c->audio_eq_mode != live_req->audio_eq_mode) {
        c->audio_auto_gain = 1.0f;
        c->audio_balance_l = 1.0f;
        c->audio_balance_r = 1.0f;
        c->audio_bass_lp_l = 0.0f;
        c->audio_bass_lp_r = 0.0f;
    }
    c->audio_eq_mode = live_req->audio_eq_mode;
    c->control_mtime_sec = st.st_mtim.tv_sec;
    c->control_mtime_nsec = st.st_mtim.tv_nsec;

    if (force ||
        fabsf(before.cas_strength - live_req->cas_strength) > 0.0001f ||
        fabsf(before.contrast_boost - live_req->contrast_boost) > 0.0001f ||
        fabsf(before.temporal_strength - live_req->temporal_strength) > 0.0001f ||
        fabsf(before.edge_stability - live_req->edge_stability) > 0.0001f ||
        fabsf(before.custom_shader_intensity - live_req->custom_shader_intensity) > 0.0001f ||
        fabsf(before.audio_cleanup_strength - live_req->audio_cleanup_strength) > 0.0001f ||
        before.audio_superres_mode != live_req->audio_superres_mode ||
        before.audio_eq_mode != live_req->audio_eq_mode ||
        before.audio_delay_ms != live_req->audio_delay_ms ||
        fabsf(before.contrast - live_req->contrast) > 0.0001f ||
        fabsf(before.saturation - live_req->saturation) > 0.0001f ||
        fabsf(before.gamma - live_req->gamma) > 0.0001f) {
        fprintf(stderr,
                "[d_native_processor] live tuning reload contrast=%.3f saturation=%.3f gamma=%.3f cas=%.3f contrast_boost=%.3f grain=%.4f temporal=%.3f edge=%.3f custom_shader=%.3f audio_cleanup=%.3f audio_superres_mode=%s audio_eq_mode=%d audio_delay_ms=%d\n",
                live_req->contrast, live_req->saturation, live_req->gamma,
                live_req->cas_strength, live_req->contrast_boost,
                live_req->grain_strength, live_req->temporal_strength, live_req->edge_stability,
                live_req->custom_shader_intensity,
                live_req->audio_cleanup_strength, audio_superres_mode_name(live_req->audio_superres_mode),
                live_req->audio_eq_mode, live_req->audio_delay_ms);
    }
}
