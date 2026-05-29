
static int json_key_eq(const char* js, const jsmntok_t* tok, const char* key) {
    if (!js || !tok || tok->type != JSMN_STRING || !key) return 0;
    int len = tok->end - tok->start;
    return (int)strlen(key) == len && strncmp(js + tok->start, key, len) == 0;
}

static void json_token_key_copy(const char* js, const jsmntok_t* tok, char dst[64]) {
    if (!dst) return;
    dst[0] = 0;
    if (!js || !tok || tok->type != JSMN_STRING || tok->start < 0 || tok->end <= tok->start) return;
    int len = tok->end - tok->start;
    size_t copy = (size_t)len < 63 ? (size_t)len : 63;
    memcpy(dst, js + tok->start, copy);
    dst[copy] = 0;
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

static int json_token_bool(const char* js, const jsmntok_t* tok, int* out) {
    if (!js || !tok || !out || tok->start < 0 || tok->end <= tok->start) return 0;
    int len = tok->end - tok->start;
    if (len == 4 && strncmp(js + tok->start, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (len == 5 && strncmp(js + tok->start, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static void json_token_copy(const char* js, const jsmntok_t* tok, char* dst, size_t dst_size) {
    if (!js || !tok || !dst || dst_size == 0 || tok->start < 0 || tok->end <= tok->start) return;
    int len = tok->end - tok->start;
    if (len < 0) return;
    size_t copy = (size_t)len < dst_size - 1 ? (size_t)len : dst_size - 1;
    memcpy(dst, js + tok->start, copy);
    dst[copy] = 0;
}

static void ascii_lower_inplace(char* s) {
    if (!s) return;
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

static int token_string_equals(const char* js, const jsmntok_t* tok, const char* value) {
    if (!js || !tok || tok->type != JSMN_STRING || !value) return 0;
    int len = tok->end - tok->start;
    return len == (int)strlen(value) && strncmp(js + tok->start, value, (size_t)len) == 0;
}

static int string_contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle) return 0;
    char h[128];
    char n[64];
    snprintf(h, sizeof(h), "%s", haystack);
    snprintf(n, sizeof(n), "%s", needle);
    ascii_lower_inplace(h);
    ascii_lower_inplace(n);
    return strstr(h, n) != NULL;
}

static int model_family_code(const char* raw) {
    if (string_contains_ci(raw, "maxine")) return DPROC_MODEL_FAMILY_MAXINE;
    if (string_contains_ci(raw, "cugan")) return DPROC_MODEL_FAMILY_CUGAN;
    if (string_contains_ci(raw, "esrgan") || string_contains_ci(raw, "realesr")) return DPROC_MODEL_FAMILY_ESRGAN;
    return DPROC_MODEL_FAMILY_NONE;
}

static int model_engine_code(const char* raw, int family_code) {
    if (string_contains_ci(raw, "tensorrt") || string_contains_ci(raw, "trt")) return DPROC_MODEL_ENGINE_TRT;
    if (string_contains_ci(raw, "nvidia-vfx") || string_contains_ci(raw, "maxine") || string_contains_ci(raw, "vfx")) return DPROC_MODEL_ENGINE_MAXINE;
    if (string_contains_ci(raw, "cuda") || string_contains_ci(raw, "nvof")) return DPROC_MODEL_ENGINE_CUDA;
    if (family_code == DPROC_MODEL_FAMILY_ESRGAN || family_code == DPROC_MODEL_FAMILY_CUGAN) return DPROC_MODEL_ENGINE_TRT;
    if (family_code == DPROC_MODEL_FAMILY_MAXINE) return DPROC_MODEL_ENGINE_MAXINE;
    return DPROC_MODEL_ENGINE_UNKNOWN;
}

static int model_kind_code(const char* raw, const char* op) {
    if (string_contains_ci(raw, "model")) return DPROC_MODEL_KIND_MODEL;
    if (string_contains_ci(raw, "motion") || string_contains_ci(op, "cadence")) return DPROC_MODEL_KIND_MOTION;
    if (string_contains_ci(raw, "filter") || string_contains_ci(raw, "shader") || string_contains_ci(raw, "post")) return DPROC_MODEL_KIND_CUDA;
    return DPROC_MODEL_KIND_UNKNOWN;
}

const char* dproc_model_family_name(int family_code) {
    switch (family_code) {
        case DPROC_MODEL_FAMILY_MAXINE: return "maxine";
        case DPROC_MODEL_FAMILY_ESRGAN: return "esrgan";
        case DPROC_MODEL_FAMILY_CUGAN: return "cugan";
        default: return "none";
    }
}

const char* dproc_model_engine_name(int engine_code) {
    switch (engine_code) {
        case DPROC_MODEL_ENGINE_TRT: return "tensorrt";
        case DPROC_MODEL_ENGINE_MAXINE: return "nvidia-vfx";
        case DPROC_MODEL_ENGINE_CUDA: return "cuda";
        default: return "unknown";
    }
}

static uint32_t fnv1a_span(const char* s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

int dproc_json_skip_token(const jsmntok_t* toks, int n, int idx) {
    if (!toks || idx < 0 || idx >= n) return idx + 1;
    const jsmntok_t* tok = &toks[idx];
    int next = idx + 1;
    if (tok->type == JSMN_ARRAY) {
        for (int i = 0; i < tok->size && next < n; i++) next = dproc_json_skip_token(toks, n, next);
    } else if (tok->type == JSMN_OBJECT) {
        for (int i = 0; i < tok->size && next < n; i++) {
            next = dproc_json_skip_token(toks, n, next);
            next = dproc_json_skip_token(toks, n, next);
        }
    }
    return next;
}
