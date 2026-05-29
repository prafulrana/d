#include "app.h"

#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include "control.h"
#include "log.h"
#include "state.h"

RuntimeState g_state = {0};
static GMainLoop *g_loop = NULL;
static AppConfig g_cfg;

static gchar *headers_without_user_agent(const gchar *headers) {
  if (!headers || !*headers) return g_strdup("");
  GString *out = g_string_new(NULL);
  gchar **lines = g_strsplit(headers, "\n", 0);
  for (guint i = 0; lines && lines[i]; i++) {
    gchar *line = lines[i];
    g_strchomp(line);
    if (!*line) continue;
    gchar *colon = strchr(line, ':');
    if (!colon) continue;
    *colon = '\0';
    gchar *name = g_strstrip(line);
    gchar *value = g_strstrip(colon + 1);
    if (g_ascii_strcasecmp(name, "User-Agent") == 0) continue;
    if (!*name || !*value) continue;
    g_string_append_printf(out, "%s: %s\r\n", name, value);
  }
  g_strfreev(lines);
  return g_string_free(out, FALSE);
}

static void add_json_value_or_empty(JsonBuilder *b, const gchar *json, const gchar *fallback) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  const gchar *text = (json && *json) ? json : fallback;
  if (text && json_parser_load_from_data(parser, text, -1, &err)) {
    json_builder_add_value(b, json_node_copy(json_parser_get_root(parser)));
  } else {
    if (err) g_error_free(err);
    json_builder_begin_object(b);
    json_builder_end_object(b);
  }
  g_object_unref(parser);
}

static gchar *stages_array_json(const GraphStage *stages, guint count) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_array(b);
  for (guint i = 0; i < count; i++) {
    if (!stages[i].id[0]) continue;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, stages[i].id);
    json_builder_set_member_name(b, "dims");
    add_json_value_or_empty(b, stages[i].dims_json, "{}");
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *out = json_generator_to_data(g, NULL);
  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out;
}

static JsonObject *stage_params_object(const GraphSpec *graph, const gchar *stage_id,
                                       JsonParser **parser_out) {
  if (parser_out) *parser_out = NULL;
  if (!graph || !stage_id) return NULL;
  for (guint i = 0; i < graph->stage_count; i++) {
    const GraphStage *stage = &graph->stages[i];
    if (g_strcmp0(stage->id, stage_id) != 0 || !stage->params_json[0]) continue;
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, stage->params_json, -1, &err)) {
      if (err) g_error_free(err);
      g_object_unref(parser);
      return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
      g_object_unref(parser);
      return NULL;
    }
    if (parser_out) *parser_out = parser;
    else g_object_unref(parser);
    return json_node_get_object(root);
  }
  return NULL;
}

static JsonObject *audio_stage_params_object(const GraphSpec *graph, const gchar *stage_id,
                                             JsonParser **parser_out) {
  if (parser_out) *parser_out = NULL;
  if (!graph || !stage_id) return NULL;
  for (guint i = 0; i < graph->audio_stage_count; i++) {
    const GraphStage *stage = &graph->audio_stages[i];
    if (g_strcmp0(stage->id, stage_id) != 0 || !stage->params_json[0]) continue;
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, stage->params_json, -1, &err)) {
      if (err) g_error_free(err);
      g_object_unref(parser);
      return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
      g_object_unref(parser);
      return NULL;
    }
    if (parser_out) *parser_out = parser;
    else g_object_unref(parser);
    return json_node_get_object(root);
  }
  return NULL;
}

static JsonObject *json_object_from_text(const gchar *json, JsonParser **parser_out) {
  if (parser_out) *parser_out = NULL;
  if (!json || !*json) return NULL;
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    if (err) g_error_free(err);
    g_object_unref(parser);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NULL;
  }
  if (parser_out) *parser_out = parser;
  else g_object_unref(parser);
  return json_node_get_object(root);
}

static gdouble json_member_double(JsonObject *obj, const gchar *name, gdouble fallback) {
  if (!obj || !json_object_has_member(obj, name)) return fallback;
  return json_object_get_double_member(obj, name);
}

static gchar *json_member_string_dup(JsonObject *obj, const gchar *name, const gchar *fallback) {
  if (!obj || !json_object_has_member(obj, name)) return g_strdup(fallback ? fallback : "");
  const gchar *value = json_object_get_string_member(obj, name);
  return g_strdup(value ? value : (fallback ? fallback : ""));
}

static gint json_member_int_clamped(JsonObject *obj, const gchar *name, gint fallback, gint min, gint max) {
  gint value = fallback;
  if (obj && json_object_has_member(obj, name)) value = (gint)json_object_get_int_member(obj, name);
  if (value < min) value = min;
  if (value > max) value = max;
  return value;
}

static gint clock_video_mode_value(JsonObject *clock) {
  gchar *mode = json_member_string_dup(clock, "videoClockMode", "source-pts");
  gchar *lc = g_ascii_strdown(mode, -1);
  const gint out = (
    strstr(lc, "sasta") ||
    strstr(lc, "receiver") ||
    strstr(lc, "monotonic") ||
    strstr(lc, "gap-squash") ||
    strstr(lc, "pts-gap")
  ) ? 1 : 0;
  g_free(lc);
  g_free(mode);
  return out;
}

static gint clock_audio_pacing_value(JsonObject *clock, gboolean is_live) {
  gchar *mode = json_member_string_dup(clock, "audioPacingMode", is_live ? "video-gated" : "source-pts");
  gchar *lc = g_ascii_strdown(mode, -1);
  const gint out = (strstr(lc, "video") || strstr(lc, "gate") || g_strcmp0(lc, "1") == 0) ? 1 : 0;
  g_free(lc);
  g_free(mode);
  return out;
}

static void native_stop_locked(void) {
  if (g_state.native_pid) {
    kill((pid_t)g_state.native_pid, SIGTERM);
    g_state.native_pid = 0;
  }
  g_state.native_running = FALSE;
}

static void native_child_watch(GPid pid, gint status, gpointer user_data) {
  (void)user_data;
  g_mutex_lock(&g_state.lock);
  if (g_state.native_pid == pid) {
    g_state.native_pid = 0;
    g_state.native_running = FALSE;
    g_state.running = FALSE;
    if (status != 0) {
      g_free(g_state.last_error);
      g_state.last_error = g_strdup_printf("native_processor_exited_status_%d", status);
    }
  }
  g_mutex_unlock(&g_state.lock);
  g_spawn_close_pid(pid);
}

static gboolean write_all_fd(int fd, const gchar *data, gsize len, gchar **error_out) {
  gsize off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (error_out) *error_out = g_strdup_printf("native_stdin_write_failed=%s", g_strerror(errno));
      return FALSE;
    }
    off += (gsize)n;
  }
  return TRUE;
}

static gchar *native_request_json(const GraphSpec *graph) {
  JsonParser *post_parser = NULL, *deband_parser = NULL, *custom_parser = NULL;
  JsonParser *dlsaa_parser = NULL, *tdn_parser = NULL;
  JsonObject *post = stage_params_object(graph, "post_vsr_finalize", &post_parser);
  JsonObject *deband = stage_params_object(graph, "deband_4k", &deband_parser);
  JsonObject *custom = stage_params_object(graph, "custom_shader", &custom_parser);
  JsonObject *dlsaa = stage_params_object(graph, "dlsaa_temporal", &dlsaa_parser);
  JsonObject *tdn = stage_params_object(graph, "temporal_denoise", &tdn_parser);
  JsonParser *ac_parser = NULL, *asr_parser = NULL, *aeq_parser = NULL, *adelay_parser = NULL;
  JsonParser *runtime_params_parser = NULL;
  JsonObject *ac = audio_stage_params_object(graph, "maxine_audio_cleanup", &ac_parser);
  JsonObject *asr = audio_stage_params_object(graph, "maxine_audio_superres", &asr_parser);
  JsonObject *aeq = audio_stage_params_object(graph, "audio_eq_profile", &aeq_parser);
  JsonObject *adelay = audio_stage_params_object(graph, "audio_delay_sync", &adelay_parser);
  JsonObject *runtime_params = json_object_from_text(graph ? graph->runtime_params_json : "{}", &runtime_params_parser);
  gchar *audio_superres_mode = json_member_string_dup(asr, "audioSuperresMode", "auto");
  gchar *runtime_state_path = json_member_string_dup(runtime_params, "runtimeStatePath", "");

  gchar *video_manifest = stages_array_json(graph->stages, graph->stage_count);
  gchar *audio_manifest = stages_array_json(graph->audio_stages, graph->audio_stage_count);
  gchar *upstream_headers = headers_without_user_agent(graph->source_headers);
  JsonParser *clock_parser = NULL;
  JsonObject *clock = json_object_from_text(graph ? graph->clock_policy_json : "{}", &clock_parser);
  const gint live_clock_mode = clock_video_mode_value(clock);
  const gint audio_pacing_mode = clock_audio_pacing_value(clock, graph ? graph->is_live : FALSE);
  const gint max_audio_lead_ms = json_member_int_clamped(clock, "maxAudioLeadMs", (graph && graph->is_live) ? 750 : 0, 0, 2000);
  const gint max_av_delta_ms = json_member_int_clamped(clock, "maxAvDeltaMs", 250, 0, 5000);
  gboolean rtsp_sink = g_str_has_prefix(graph->sink_uri, "rtsp://") || g_str_has_prefix(graph->sink_uri, "rtsps://");

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "url"); json_builder_add_string_value(b, graph->source_uri);
  json_builder_set_member_name(b, "headers"); json_builder_add_string_value(b, upstream_headers);
  json_builder_set_member_name(b, "user_agent"); json_builder_add_string_value(b, "Kodi/21.2 (Linux; Android 12; Pixel 7) Version/21.2-(21.2-Omega)");
  json_builder_set_member_name(b, "output_url"); json_builder_add_string_value(b, graph->sink_uri);
  json_builder_set_member_name(b, "output_format"); json_builder_add_string_value(b, rtsp_sink ? "rtsp" : "mpegts");
  json_builder_set_member_name(b, "perf_ring_path"); json_builder_add_string_value(b, graph->perf_ring_path);
  json_builder_set_member_name(b, "runtime_state_path"); json_builder_add_string_value(b, runtime_state_path);
  json_builder_set_member_name(b, "is_live"); json_builder_add_boolean_value(b, graph->is_live);
  json_builder_set_member_name(b, "output_fps"); json_builder_add_int_value(b, graph->output_fps);
  json_builder_set_member_name(b, "d_pipeline_json"); add_json_value_or_empty(b, graph->d_pipeline_json, "{}");
  json_builder_set_member_name(b, "bitrate_bps"); json_builder_add_int_value(b, graph->bitrate_bps);
  json_builder_set_member_name(b, "max_bitrate_bps"); json_builder_add_int_value(b, graph->max_bitrate_bps);
  json_builder_set_member_name(b, "live_output_cushion_ms"); json_builder_add_int_value(b, graph->output_queue_ms);
  json_builder_set_member_name(b, "contrast"); json_builder_add_double_value(b, json_member_double(post, "contrast", 1.06));
  json_builder_set_member_name(b, "saturation"); json_builder_add_double_value(b, json_member_double(post, "saturation", 1.07));
  json_builder_set_member_name(b, "gamma"); json_builder_add_double_value(b, json_member_double(post, "gamma", 0.98));
  json_builder_set_member_name(b, "cas_strength"); json_builder_add_double_value(b, json_member_double(post, "casStrength", 0.68));
  json_builder_set_member_name(b, "contrast_boost"); json_builder_add_double_value(b, json_member_double(post, "contrastBoost", 0.52));
  json_builder_set_member_name(b, "grain_strength"); json_builder_add_double_value(b, json_member_double(post, "grainStrength", 0.0));
  json_builder_set_member_name(b, "temporal_strength"); json_builder_add_double_value(b, json_member_double(dlsaa, "temporalStrength", 0.42));
  json_builder_set_member_name(b, "edge_stability"); json_builder_add_double_value(b, json_member_double(dlsaa, "edgeStability", 1.08));
  json_builder_set_member_name(b, "deband_strength"); json_builder_add_double_value(b, json_member_double(deband, "debandStrength", 0.5));
  json_builder_set_member_name(b, "custom_shader_intensity"); json_builder_add_double_value(b, json_member_double(custom, "customShaderIntensity", 0.0));
  json_builder_set_member_name(b, "temporal_denoise_strength"); json_builder_add_double_value(b, json_member_double(tdn, "temporalDenoiseStrength", 0.0));
  json_builder_set_member_name(b, "temporal_denoise_luma_max"); json_builder_add_double_value(b, json_member_double(tdn, "temporalDenoiseLumaMax", 0.0));
  json_builder_set_member_name(b, "audio_cleanup_strength"); json_builder_add_double_value(b, json_member_double(ac, "audioCleanupStrength", 0.0));
  json_builder_set_member_name(b, "audio_superres_mode"); json_builder_add_string_value(b, audio_superres_mode);
  json_builder_set_member_name(b, "audio_passthrough"); json_builder_add_int_value(b, 0);
  json_builder_set_member_name(b, "audio_eq_mode"); json_builder_add_int_value(b, (gint)json_member_double(aeq, "audioEqMode", 1));
  json_builder_set_member_name(b, "audio_delay_ms"); json_builder_add_int_value(b, (gint)json_member_double(adelay, "audioDelayMs", 0));
  json_builder_set_member_name(b, "live_clock_mode"); json_builder_add_int_value(b, live_clock_mode);
  json_builder_set_member_name(b, "audio_pacing_mode"); json_builder_add_int_value(b, audio_pacing_mode);
  json_builder_set_member_name(b, "max_audio_lead_ms"); json_builder_add_int_value(b, max_audio_lead_ms);
  json_builder_set_member_name(b, "max_av_delta_ms"); json_builder_add_int_value(b, max_av_delta_ms);
  json_builder_set_member_name(b, "pipeline_manifest_json"); add_json_value_or_empty(b, video_manifest, "[]");
  json_builder_set_member_name(b, "audio_pipeline_manifest_json"); add_json_value_or_empty(b, audio_manifest, "[]");
  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *out = json_generator_to_data(g, NULL);
  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);

  if (post_parser) g_object_unref(post_parser);
  if (deband_parser) g_object_unref(deband_parser);
  if (custom_parser) g_object_unref(custom_parser);
  if (dlsaa_parser) g_object_unref(dlsaa_parser);
  if (tdn_parser) g_object_unref(tdn_parser);
  if (ac_parser) g_object_unref(ac_parser);
  if (asr_parser) g_object_unref(asr_parser);
  if (aeq_parser) g_object_unref(aeq_parser);
  if (adelay_parser) g_object_unref(adelay_parser);
  if (runtime_params_parser) g_object_unref(runtime_params_parser);
  if (clock_parser) g_object_unref(clock_parser);
  g_free(audio_superres_mode);
  g_free(runtime_state_path);
  g_free(video_manifest);
  g_free(audio_manifest);
  g_free(upstream_headers);
  return out;
}

static gboolean app_select_native_graph(const GraphSpec *graph, gchar **error_out) {
  gchar *request = native_request_json(graph);
  GPid pid = 0;
  gint stdin_fd = -1;
  gchar *argv[] = { "/opt/dgst/d_native_processor", NULL };
  GError *err = NULL;

  g_mutex_lock(&g_state.lock);
  native_stop_locked();
  g_mutex_unlock(&g_state.lock);

  if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, &stdin_fd, NULL, NULL, &err)) {
    if (error_out) *error_out = g_strdup(err ? err->message : "native_spawn_failed");
    if (err) g_error_free(err);
    g_free(request);
    return FALSE;
  }
  g_child_watch_add(pid, native_child_watch, NULL);
  gchar *line = g_strdup_printf("%s\n", request);
  gboolean wrote = write_all_fd(stdin_fd, line, strlen(line), error_out);
  close(stdin_fd);
  g_free(line);
  g_free(request);
  if (!wrote) {
    kill((pid_t)pid, SIGTERM);
    return FALSE;
  }

  g_mutex_lock(&g_state.lock);
  g_state.native_pid = pid;
  g_state.native_running = TRUE;
  g_state.graph = *graph;
  g_state.running = TRUE;
  g_free(g_state.last_error);
  g_state.last_error = NULL;
  g_mutex_unlock(&g_state.lock);

  LOG_INF("selected native graph runtime=%s program=%s source=%s sink=%s stages=%u audio=%u pid=%d",
          graph->runtime_name, graph->program_name, graph->source_uri, graph->sink_uri,
          graph->stage_count, graph->audio_stage_count, (int)pid);
  return TRUE;
}

gboolean app_select_graph(const GraphSpec *graph, gchar **error_out) {
  if (!graph_spec_validate_links(graph, error_out)) return FALSE;
  return app_select_native_graph(graph, error_out);
}

void app_stop_graph(void) {
  g_mutex_lock(&g_state.lock);
  native_stop_locked();
  g_state.running = FALSE;
  g_mutex_unlock(&g_state.lock);
}

gchar *app_status_json(void) {
  g_mutex_lock(&g_state.lock);
  gchar *out = graph_spec_summary_json(&g_state.graph,
                                       g_state.running ? "running" : "idle",
                                       g_state.last_error);
  g_mutex_unlock(&g_state.lock);
  return out;
}

static gboolean on_signal_cb(gpointer data) {
  GMainLoop *loop = (GMainLoop *)data;
  LOG_INF("Signal received; shutting down");
  g_main_loop_quit(loop);
  return G_SOURCE_CONTINUE;
}

gboolean app_setup(const AppConfig *cfg) {
  g_cfg.ctrl_port = cfg->ctrl_port;
  g_cfg.default_graph = g_strdup(cfg->default_graph);
  g_cfg.mediamtx_rtsp_url = g_strdup(cfg->mediamtx_rtsp_url);
  g_cfg.public_playback_url = g_strdup(cfg->public_playback_url);
  g_mutex_init(&g_state.lock);
  graph_spec_init(&g_state.graph);

  gchar *error = NULL;
  GraphSpec graph;
  if (graph_spec_load_file(cfg->default_graph, &graph, &error)) {
    if (!graph.sink_uri[0]) g_strlcpy(graph.sink_uri, cfg->mediamtx_rtsp_url, sizeof(graph.sink_uri));
    g_mutex_lock(&g_state.lock);
    g_state.graph = graph;
    g_mutex_unlock(&g_state.lock);
    if (graph.source_uri[0]) {
      if (!app_select_graph(&graph, &error)) {
        LOG_WRN("default graph did not start: %s", error ? error : "unknown");
      }
    }
  } else {
    LOG_WRN("default graph not loaded: %s", error ? error : "unknown");
  }
  g_free(error);

  (void)g_thread_new("ctrl_http", control_http_thread, GUINT_TO_POINTER(cfg->ctrl_port));
  g_loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGINT, on_signal_cb, g_loop);
  g_unix_signal_add(SIGTERM, on_signal_cb, g_loop);
  LOG_INF("control API listening on 0.0.0.0:%u", cfg->ctrl_port);
  return TRUE;
}

void app_loop(void) {
  if (g_loop) g_main_loop_run(g_loop);
}

void app_teardown(void) {
  app_stop_graph();
  if (g_loop) {
    g_main_loop_unref(g_loop);
    g_loop = NULL;
  }
  cleanup_config(&g_cfg);
}
