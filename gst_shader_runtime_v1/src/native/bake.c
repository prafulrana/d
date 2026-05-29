// d bake CLI front-end.
//
// Runtime implementation lives in bake_runtime.c and stage dispatch lives in
// pipeline_stages.c. This file owns only process setup plus the JSON stdin
// contract so bake.c stays small and the worker remains manifest-driven.

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <libavutil/log.h>

#include "bake.h"
#include "generated/d_pipeline_contract.h"
#include "jsmn.h"
#include "pipeline_manifest.h"

static int jsmn_key_eq(const char* js, const jsmntok_t* tok, const char* key) {
    if (!js || !tok || tok->type != JSMN_STRING || !key) return 0;
    int len = tok->end - tok->start;
    return (int)strlen(key) == len && strncmp(js + tok->start, key, len) == 0;
}

static int json_bool_token(const char* js, const jsmntok_t* tok) {
    if (!js || !tok || tok->start < 0 || tok->end <= tok->start) return 0;
    char c = (char)tolower((unsigned char)js[tok->start]);
    return c == 't' || c == '1' || c == 'y';
}

static int json_live_clock_mode(const char* js, const jsmntok_t* tok) {
    if (!js || !tok || tok->start < 0 || tok->end <= tok->start) return 0;
    int len = tok->end - tok->start;
    if (len == 1 && js[tok->start] == '1') return 1;
    char tmp[64];
    int copy = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    for (int i = 0; i < copy; i++) tmp[i] = (char)tolower((unsigned char)js[tok->start + i]);
    tmp[copy] = 0;
    return strstr(tmp, "true") || strstr(tmp, "decode") || strstr(tmp, "sasta") || strstr(tmp, "gap");
}

static int json_audio_pacing_mode(char* line, const jsmntok_t* tok) {
    if (!line || !tok || tok->start < 0 || tok->end <= tok->start) return DPROC_AUDIO_PACING_SOURCE_PTS;
    int len = tok->end - tok->start;
    if (len == 1 && line[tok->start] == '1') return DPROC_AUDIO_PACING_VIDEO_GATED;
    char tmp[64];
    int copy = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    for (int i = 0; i < copy; i++) tmp[i] = (char)tolower((unsigned char)line[tok->start + i]);
    tmp[copy] = 0;
    return (strstr(tmp, "true") || strstr(tmp, "video") || strstr(tmp, "gate") || strstr(tmp, "live"))
        ? DPROC_AUDIO_PACING_VIDEO_GATED
        : DPROC_AUDIO_PACING_SOURCE_PTS;
}

static float clamp_float(float value, float min, float max) {
    if (!isfinite(value)) return min;
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int parse_audio_superres_mode(const char* value) {
    if (!value || !*value) return DPROC_AUDIO_SR_AUTO;
    if (strcasecmp(value, "force") == 0 || strcasecmp(value, "forced") == 0 || strcmp(value, "1") == 0) {
        return DPROC_AUDIO_SR_FORCE;
    }
    if (strcasecmp(value, "off") == 0 || strcasecmp(value, "none") == 0 || strcmp(value, "2") == 0) {
        return DPROC_AUDIO_SR_OFF;
    }
    return DPROC_AUDIO_SR_AUTO;
}

static void configure_ffmpeg_log_level(void) {
    const char* raw = getenv("DPROC_FFMPEG_LOG_LEVEL");
    int level = AV_LOG_WARNING;
    if (raw && *raw) {
        if (strcasecmp(raw, "quiet") == 0) level = AV_LOG_QUIET;
        else if (strcasecmp(raw, "error") == 0) level = AV_LOG_ERROR;
        else if (strcasecmp(raw, "warning") == 0 || strcasecmp(raw, "warn") == 0) level = AV_LOG_WARNING;
        else if (strcasecmp(raw, "info") == 0) level = AV_LOG_INFO;
        else if (strcasecmp(raw, "debug") == 0) level = AV_LOG_DEBUG;
    }
    av_log_set_level(level);
}

static void set_request_defaults(BakeRequest* req) {
    memset(req, 0, sizeof(*req));
    req->fps = DPIPELINE_FPS_DEFAULT;
    req->bitrate_bps = 24000000;
    req->max_bitrate_bps = 32000000;
    req->live_output_cushion_ms = 3000;
    req->contrast = 1.06f;
    req->saturation = 1.07f;
    req->gamma = 0.98f;
    req->cas_strength = 0.68f;
    req->contrast_boost = 0.52f;
    req->grain_strength = 0.0f;
    req->temporal_strength = 0.42f;
    req->edge_stability = 1.08f;
    req->custom_shader_intensity = 0.0f;
    req->audio_cleanup_strength = 0.0f;
    req->audio_superres_mode = DPROC_AUDIO_SR_AUTO;
    req->audio_passthrough = 0;
    req->audio_eq_mode = 1;
    req->audio_delay_ms = 0;
    req->live_clock_mode = 0;
    req->audio_pacing_mode = DPROC_AUDIO_PACING_SOURCE_PTS;
    req->max_audio_lead_ms = 750;
    req->max_av_delta_ms = 250;
}

static void clamp_request(BakeRequest* req) {
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
    req->audio_pacing_mode = req->audio_pacing_mode == DPROC_AUDIO_PACING_VIDEO_GATED
        ? DPROC_AUDIO_PACING_VIDEO_GATED
        : DPROC_AUDIO_PACING_SOURCE_PTS;
    if (req->max_audio_lead_ms < 0) req->max_audio_lead_ms = 0;
    if (req->max_audio_lead_ms > 2000) req->max_audio_lead_ms = 2000;
    if (req->max_av_delta_ms < 0) req->max_av_delta_ms = 0;
    if (req->max_av_delta_ms > 5000) req->max_av_delta_ms = 5000;
    if (req->live_output_cushion_ms < 0) req->live_output_cushion_ms = 0;
    if (req->live_output_cushion_ms > 60000) req->live_output_cushion_ms = 60000;
}

static void json_unescape_string_inplace(char* s) {
    if (!s) return;
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r != '\\') {
            *w++ = *r++;
            continue;
        }
        r++;
        switch (*r) {
            case 'n': *w++ = '\n'; r++; break;
            case 'r': *w++ = '\r'; r++; break;
            case 't': *w++ = '\t'; r++; break;
            case '"': *w++ = '"'; r++; break;
            case '\\': *w++ = '\\'; r++; break;
            case '/': *w++ = '/'; r++; break;
            case 'u':
                *w++ = '?';
                r++;
                for (int i = 0; i < 4 && *r; i++) r++;
                break;
            case '\0':
                *w++ = '\\';
                break;
            default:
                *w++ = *r++;
                break;
        }
    }
    *w = '\0';
}

static void set_string_value(char* line, const jsmntok_t* tok, const char** dst) {
    line[tok->end] = 0;
    char* value = line + tok->start;
    json_unescape_string_inplace(value);
    *dst = value;
}

static int parse_audio_superres_token(char* line, const jsmntok_t* tok) {
    if (!line || !tok || tok->start < 0 || tok->end <= tok->start) return DPROC_AUDIO_SR_AUTO;
    line[tok->end] = 0;
    char* value = line + tok->start;
    json_unescape_string_inplace(value);
    return parse_audio_superres_mode(value);
}

static void set_manifest_value(char* line, const jsmntok_t* toks, int n, int* i, const char** dst) {
    int val = *i + 1;
    if (val < n && toks[val].type == JSMN_ARRAY) {
        line[toks[val].end] = 0;
        *dst = line + toks[val].start;
        *i = dproc_json_skip_token(toks, n, val) - 1;
    } else {
        *i = val;
    }
}

static void set_json_value(char* line, const jsmntok_t* toks, int n, int* i, const char** dst) {
    int val = *i + 1;
    if (val < n && (toks[val].type == JSMN_OBJECT || toks[val].type == JSMN_ARRAY)) {
        line[toks[val].end] = 0;
        *dst = line + toks[val].start;
        *i = dproc_json_skip_token(toks, n, val) - 1;
    } else {
        *i = val;
    }
}

static int parse_request_line(char* line, BakeRequest* req, int* prep_only) {
    jsmn_parser p;
    jsmn_init(&p);
    jsmntok_t toks[8192];
    int n = jsmn_parse(&p, line, strlen(line), toks, (unsigned)(sizeof(toks) / sizeof(toks[0])));
    if (n < 1 || toks[0].type != JSMN_OBJECT) return -1;

    for (int i = 1; i < n; i++) {
        if (toks[i].type != JSMN_STRING || i + 1 >= n) continue;
        jsmntok_t* val = &toks[i + 1];
        if (jsmn_key_eq(line, &toks[i], "url")) { set_string_value(line, val, &req->url); i++; }
        else if (jsmn_key_eq(line, &toks[i], "http_proxy")) { set_string_value(line, val, &req->http_proxy); i++; }
        else if (jsmn_key_eq(line, &toks[i], "user_agent")) { set_string_value(line, val, &req->user_agent); i++; }
        else if (jsmn_key_eq(line, &toks[i], "headers") || jsmn_key_eq(line, &toks[i], "upstream_headers")) { set_string_value(line, val, &req->headers); i++; }
        else if (jsmn_key_eq(line, &toks[i], "output_url") || jsmn_key_eq(line, &toks[i], "sink_uri")) { set_string_value(line, val, &req->output_url); i++; }
        else if (jsmn_key_eq(line, &toks[i], "output_format") || jsmn_key_eq(line, &toks[i], "sink_format")) { set_string_value(line, val, &req->output_format); i++; }
        else if (jsmn_key_eq(line, &toks[i], "control_path")) { set_string_value(line, val, &req->control_path); i++; }
        else if (jsmn_key_eq(line, &toks[i], "trt_engine_path_480")) { set_string_value(line, val, &req->trt_engine_path_480); i++; }
        else if (jsmn_key_eq(line, &toks[i], "trt_engine_path_720")) { set_string_value(line, val, &req->trt_engine_path_720); i++; }
        else if (jsmn_key_eq(line, &toks[i], "trt_engine_path_1080")) { set_string_value(line, val, &req->trt_engine_path_1080); i++; }
        else if (jsmn_key_eq(line, &toks[i], "runtime_state_path")) { set_string_value(line, val, &req->runtime_state_path); i++; }
        else if (jsmn_key_eq(line, &toks[i], "perf_ring_path")) { set_string_value(line, val, &req->perf_ring_path); i++; }
        else if (jsmn_key_eq(line, &toks[i], "pipeline_manifest_json")) { set_manifest_value(line, toks, n, &i, &req->pipeline_manifest_json); }
        else if (jsmn_key_eq(line, &toks[i], "audio_pipeline_manifest_json")) { set_manifest_value(line, toks, n, &i, &req->audio_pipeline_manifest_json); }
        else if (jsmn_key_eq(line, &toks[i], "start_seconds") || jsmn_key_eq(line, &toks[i], "start")) { req->start_seconds = atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "duration") || jsmn_key_eq(line, &toks[i], "duration_seconds")) { req->duration_seconds = atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "bitrate") || jsmn_key_eq(line, &toks[i], "bitrate_bps")) { req->bitrate_bps = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "maxbitrate") || jsmn_key_eq(line, &toks[i], "max_bitrate_bps")) { req->max_bitrate_bps = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "live_output_cushion_ms") || jsmn_key_eq(line, &toks[i], "output_queue_ms")) { req->live_output_cushion_ms = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "fps") || jsmn_key_eq(line, &toks[i], "output_fps")) { req->fps = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "contrast")) { req->contrast = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "saturation")) { req->saturation = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "gamma")) { req->gamma = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "cas_strength")) { req->cas_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "contrast_boost")) { req->contrast_boost = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "grain_strength")) { req->grain_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "temporal_strength")) { req->temporal_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "edge_stability")) { req->edge_stability = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "custom_shader_intensity")) { req->custom_shader_intensity = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_cleanup_strength")) { req->audio_cleanup_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_superres_mode")) { req->audio_superres_mode = parse_audio_superres_token(line, val); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_passthrough")) { req->audio_passthrough = json_bool_token(line, val); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_eq_mode")) { req->audio_eq_mode = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_delay_ms")) { req->audio_delay_ms = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "live_clock_mode") || jsmn_key_eq(line, &toks[i], "audio_clock_mode")) { req->live_clock_mode = json_live_clock_mode(line, val); i++; }
        else if (jsmn_key_eq(line, &toks[i], "audio_pacing_mode")) { req->audio_pacing_mode = json_audio_pacing_mode(line, val); i++; }
        else if (jsmn_key_eq(line, &toks[i], "max_audio_lead_ms")) { req->max_audio_lead_ms = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "max_av_delta_ms")) { req->max_av_delta_ms = atoi(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "is_live") || jsmn_key_eq(line, &toks[i], "live")) { req->is_live = json_bool_token(line, val); i++; }
        else if (jsmn_key_eq(line, &toks[i], "d_pipeline_json")) { set_json_value(line, toks, n, &i, &req->d_pipeline_json); }
        else if (jsmn_key_eq(line, &toks[i], "deband_strength")) { req->deband_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "temporal_denoise_strength")) { req->temporal_denoise_strength = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "temporal_denoise_luma_max")) { req->temporal_denoise_luma_max = (float)atof(line + val->start); i++; }
        else if (jsmn_key_eq(line, &toks[i], "prep_only")) { if (prep_only) *prep_only = json_bool_token(line, val); i++; }
        else {
            line[toks[i].end] = 0;
            fprintf(stderr, "[d_native_processor] unknown_request_field=%s\n", line + toks[i].start);
            return -1;
        }
    }
    clamp_request(req);
    if (req->d_pipeline_json && req->d_pipeline_json[0] && strcmp(req->d_pipeline_json, "{}") != 0) {
        char err[256] = {0};
        if (dproc_model_pipeline_parse(req->model_stages, &req->model_stage_count,
                                       req->d_pipeline_json, err, sizeof(err)) < 0) {
            fprintf(stderr, "[d_native_processor] d_pipeline_parse_failed=%s\n", err[0] ? err : "unknown");
            return -1;
        }
    }
    if (req->model_stage_count <= 0) {
        fprintf(stderr, "[d_native_processor] d_pipeline_json_required\n");
        return -1;
    }
    return 0;
}

static void prefault_ngx_cache(void) {
    const char* home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/.nv/ComputeCache", home);
    DIR* dir = opendir(path);
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') continue;
        char file[768];
        snprintf(file, sizeof(file), "%s/%s", path, ent->d_name);
        int fd = open(file, O_RDONLY);
        if (fd < 0) continue;
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
    }
    closedir(dir);
}

static void log_request(const char* prefix, int prep_only, const BakeRequest* req) {
    fprintf(stderr,
            "[d_native_processor] %s prep_only=%d url_present=%d live=%d proxy_present=%d headers_present=%d output_present=%d control_present=%d start=%.3f duration=%.3f fps=%d bitrate=%d max=%d output_queue_ms=%d model_stages=%d temporal=%.3f custom_shader=%.3f audio_cleanup=%.3f audio_superres_mode=%d audio_passthrough=%d audio_eq_mode=%d audio_delay_ms=%d live_clock_mode=%d audio_pacing_mode=%d max_audio_lead_ms=%d max_av_delta_ms=%d video_manifest=%d audio_manifest=%d d_pipeline=%d maxine_audio=%s\n",
            prefix,
            prep_only,
            req->url && *req->url ? 1 : 0,
            req->is_live,
            req->http_proxy && *req->http_proxy ? 1 : 0,
            req->headers && *req->headers ? 1 : 0,
            req->output_url && *req->output_url ? 1 : 0,
            req->control_path && *req->control_path ? 1 : 0,
            req->start_seconds,
            req->duration_seconds,
            req->fps,
            req->bitrate_bps,
            req->max_bitrate_bps,
            req->live_output_cushion_ms,
            req->model_stage_count,
            req->temporal_strength,
            req->custom_shader_intensity,
            req->audio_cleanup_strength,
            req->audio_superres_mode,
            req->audio_passthrough,
            req->audio_eq_mode,
            req->audio_delay_ms,
            req->live_clock_mode,
            req->audio_pacing_mode,
            req->max_audio_lead_ms,
            req->max_av_delta_ms,
            req->pipeline_manifest_json && *req->pipeline_manifest_json ? 1 : 0,
            req->audio_pipeline_manifest_json && *req->audio_pipeline_manifest_json ? 1 : 0,
            req->d_pipeline_json && *req->d_pipeline_json ? 1 : 0,
            req->audio_passthrough ? "aac_transport_no_maxine" : "required");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    setlinebuf(stderr);
    signal(SIGPIPE, SIG_IGN);
    configure_ffmpeg_log_level();

    struct rlimit lim = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &lim);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[d_native_processor] mlockall non-fatal: %s\n", strerror(errno));
    }
    prefault_ngx_cache();

    BakeWorker w;
    memset(&w, 0, sizeof(w));
    BakeRequest req;
    set_request_defaults(&req);
    fprintf(stderr, "[d_native_processor] stdin ready, awaiting request\n");

    char* line = NULL;
    char* stream_line = NULL;
    size_t cap = 0;
    size_t stream_cap = 0;
    int prep_only = 0;
    int rc = 0;

    if (getline(&line, &cap, stdin) <= 0) {
        fprintf(stderr, "[d_native_processor] no request on stdin\n");
        rc = 2;
        goto done;
    }
    if (parse_request_line(line, &req, &prep_only) < 0) {
        fprintf(stderr, "[d_native_processor] bad_json\n");
        rc = 3;
        goto done;
    }
    if (!req.url) {
        fprintf(stderr, "[d_native_processor] missing_url\n");
        rc = 4;
        goto done;
    }
    log_request("request parsed", prep_only, &req);

    if (prep_only) {
        BakeResult prep_res;
        int prc = bake_prepare(&w, &req, &prep_res);
        if (prc != 0) {
            fprintf(stderr, "[d_native_processor] prepare_failed: %s\n", prep_res.error);
            rc = 5;
            goto done;
        }
        fprintf(stderr, "[d_native_processor] prepared, awaiting stream\n");
        if (getline(&stream_line, &stream_cap, stdin) <= 0) {
            fprintf(stderr, "[d_native_processor] no stream request on stdin\n");
            bake_release_prepared(&w);
            rc = 6;
            goto done;
        }
        if (parse_request_line(stream_line, &req, NULL) < 0) {
            fprintf(stderr, "[d_native_processor] bad_stream_json\n");
            bake_release_prepared(&w);
            rc = 7;
            goto done;
        }
        log_request("prepared stream request", prep_only, &req);
    }

    BakeResult res;
    rc = prep_only ? bake_run_prepared(&w, &req, &res) : bake_run(&w, &req, &res);
    fprintf(stderr, "[d_native_processor] done ok=%d frames_in=%d frames_out=%d bytes=%lld audio_packets=%lld audio_frames=%lld audio_maxine_runs=%lld audio_encoded_packets=%lld audio_bytes=%lld elapsed_s=%.3f loop_fps=%.1f err=%s\n",
            res.ok, res.frames_in, res.frames_out, res.bytes_out,
            res.audio_packets, res.audio_frames, res.audio_maxine_runs,
            res.audio_encoded_packets, res.audio_bytes_out,
            res.elapsed_seconds,
            res.loop_seconds > 0 ? res.frames_in / res.loop_seconds : 0,
            res.ok ? "-" : res.error);

done:
    free(stream_line);
    free(line);
    bake_worker_destroy(&w);
    return rc;
}
