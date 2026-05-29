#include "graph.h"

#include <json-glib/json-glib.h>
#include <string.h>

static void copy_member_string(JsonObject *obj, const gchar *name, gchar *dst, size_t dst_size) {
  if (!json_object_has_member(obj, name)) return;
  const gchar *value = json_object_get_string_member(obj, name);
  if (!value) return;
  g_strlcpy(dst, value, dst_size);
}

static guint member_uint(JsonObject *obj, const gchar *name, guint fallback) {
  if (!json_object_has_member(obj, name)) return fallback;
  gint64 value = json_object_get_int_member(obj, name);
  return value > 0 ? (guint)value : fallback;
}

static guint object_uint(JsonObject *obj, const gchar *name, guint fallback) {
  if (!obj || !json_object_has_member(obj, name)) return fallback;
  gint64 value = json_object_get_int_member(obj, name);
  return value > 0 ? (guint)value : fallback;
}

static void copy_member_json(JsonObject *obj, const gchar *name, gchar *dst, size_t dst_size, const gchar *fallback) {
  if (!dst || dst_size == 0) return;
  g_strlcpy(dst, fallback ? fallback : "", dst_size);
  if (!obj || !json_object_has_member(obj, name)) return;
  JsonNode *node = json_object_get_member(obj, name);
  if (!node) return;
  JsonGenerator *g = json_generator_new();
  json_generator_set_root(g, node);
  gchar *text = json_generator_to_data(g, NULL);
  if (text) g_strlcpy(dst, text, dst_size);
  g_free(text);
  g_object_unref(g);
}

static void add_json_value(JsonBuilder *b, const gchar *json, const gchar *fallback) {
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

static gchar *json_member_to_text(JsonObject *obj, const gchar *name, const gchar *fallback) {
  if (!obj || !json_object_has_member(obj, name)) return g_strdup(fallback ? fallback : "{}");
  JsonNode *node = json_object_get_member(obj, name);
  if (!node) return g_strdup(fallback ? fallback : "{}");
  JsonGenerator *g = json_generator_new();
  json_generator_set_root(g, node);
  gchar *text = json_generator_to_data(g, NULL);
  g_object_unref(g);
  return text ? text : g_strdup(fallback ? fallback : "{}");
}

static gchar *d_pipeline_link_plan_json(const gchar *json) {
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  const gchar *text = (json && *json) ? json : "{}";
  if (!json_parser_load_from_data(parser, text, -1, &err)) {
    if (err) g_error_free(err);
    g_object_unref(parser);
    return g_strdup("{\"ok\":false,\"errors\":[{\"error\":\"d_pipeline_json_parse_failed\"}],\"warnings\":[],\"links\":[]}");
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return g_strdup("{\"ok\":false,\"errors\":[{\"error\":\"d_pipeline_json_not_object\"}],\"warnings\":[],\"links\":[]}");
  }
  JsonObject *obj = json_node_get_object(root);
  gchar *out = json_member_to_text(obj, "linkPlan", "{\"ok\":true,\"errors\":[],\"warnings\":[],\"links\":[]}");
  g_object_unref(parser);
  return out;
}

void graph_spec_init(GraphSpec *spec) {
  D_GRAPH_SPEC_INIT_DEFAULTS(spec);
}

gboolean graph_spec_parse_json(const gchar *json, GraphSpec *spec, gchar **error_out) {
  graph_spec_init(spec);
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  if (!json_parser_load_from_data(parser, json, -1, &err)) {
    if (error_out) *error_out = g_strdup(err ? err->message : "json_parse_failed");
    if (err) g_error_free(err);
    g_object_unref(parser);
    return FALSE;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    if (error_out) *error_out = g_strdup("json_root_not_object");
    g_object_unref(parser);
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);
  GList *members = json_object_get_members(obj);
  for (GList *it = members; it; it = it->next) {
    const gchar *key = (const gchar *)it->data;
    if (!D_GRAPH_SPEC_IS_ALLOWED_KEY(key)) {
      if (error_out) *error_out = g_strdup_printf("unknown_runtime_graph_field=%s", key ? key : "");
      g_list_free(members);
      g_object_unref(parser);
      return FALSE;
    }
  }
  g_list_free(members);
  D_GRAPH_SPEC_PARSE_JSON_FIELDS(obj, spec);

  if (json_object_has_member(obj, "stages")) {
    JsonArray *stages = json_object_get_array_member(obj, "stages");
    guint n = MIN(json_array_get_length(stages), DGST_MAX_STAGES);
    for (guint i = 0; i < n; i++) {
      JsonObject *stage = json_array_get_object_element(stages, i);
      if (!stage) continue;
      GraphStage *dst = &spec->stages[spec->stage_count];
      copy_member_string(stage, "id", dst->id, sizeof(dst->id));
      copy_member_string(stage, "kind", dst->kind, sizeof(dst->kind));
      copy_member_string(stage, "plugin", dst->plugin, sizeof(dst->plugin));
      copy_member_json(stage, "dims", dst->dims_json, sizeof(dst->dims_json), "");
      copy_member_json(stage, "params", dst->params_json, sizeof(dst->params_json), "{}");
      copy_member_json(stage, "dsl", dst->dsl_json, sizeof(dst->dsl_json), "{}");
      if (g_strcmp0(dst->id, "upscaler") == 0 &&
          json_object_has_member(stage, "dims")) {
        JsonObject *dims = json_object_get_object_member(stage, "dims");
        JsonObject *in = dims && json_object_has_member(dims, "in")
          ? json_object_get_object_member(dims, "in")
          : NULL;
        spec->processing_width = object_uint(in, "w", spec->processing_width);
        spec->processing_height = object_uint(in, "h", spec->processing_height);
      }
      spec->stage_count++;
    }
  }
  if (json_object_has_member(obj, "audioStages")) {
    JsonArray *stages = json_object_get_array_member(obj, "audioStages");
    guint n = MIN(json_array_get_length(stages), DGST_MAX_STAGES);
    for (guint i = 0; i < n; i++) {
      JsonObject *stage = json_array_get_object_element(stages, i);
      if (!stage) continue;
      GraphStage *dst = &spec->audio_stages[spec->audio_stage_count];
      copy_member_string(stage, "id", dst->id, sizeof(dst->id));
      copy_member_string(stage, "kind", dst->kind, sizeof(dst->kind));
      copy_member_string(stage, "plugin", dst->plugin, sizeof(dst->plugin));
      copy_member_json(stage, "dims", dst->dims_json, sizeof(dst->dims_json), "");
      copy_member_json(stage, "params", dst->params_json, sizeof(dst->params_json), "{}");
      copy_member_json(stage, "dsl", dst->dsl_json, sizeof(dst->dsl_json), "{}");
      spec->audio_stage_count++;
    }
  }
  g_object_unref(parser);
  return TRUE;
}

gboolean graph_spec_load_file(const gchar *path, GraphSpec *spec, gchar **error_out) {
  gchar *text = NULL;
  GError *err = NULL;
  if (!g_file_get_contents(path, &text, NULL, &err)) {
    if (error_out) *error_out = g_strdup(err ? err->message : "graph_read_failed");
    if (err) g_error_free(err);
    return FALSE;
  }
  gboolean ok = graph_spec_parse_json(text, spec, error_out);
  g_free(text);
  return ok;
}

gboolean graph_spec_validate_links(const GraphSpec *spec, gchar **error_out) {
  if (!spec || !spec->d_pipeline_json[0] || g_strcmp0(spec->d_pipeline_json, "{}") == 0) {
    if (error_out) *error_out = g_strdup("graph.link_error:d_pipeline_required");
    return FALSE;
  }
  gchar *plan = d_pipeline_link_plan_json(spec ? spec->d_pipeline_json : "{}");
  JsonParser *parser = json_parser_new();
  GError *err = NULL;
  gboolean ok = FALSE;
  if (json_parser_load_from_data(parser, plan, -1, &err)) {
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *obj = json_node_get_object(root);
      ok = !json_object_has_member(obj, "ok") || json_object_get_boolean_member(obj, "ok");
      if (!ok && error_out) {
        gchar *errors = json_member_to_text(obj, "errors", "[]");
        *error_out = g_strdup_printf("graph.link_error=%s", errors);
        g_free(errors);
      }
    }
  }
  if (err) g_error_free(err);
  g_object_unref(parser);
  g_free(plan);
  if (!ok && error_out && !*error_out) *error_out = g_strdup("graph.link_error");
  return ok;
}

gchar *graph_spec_to_runtime_json(const GraphSpec *spec) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  D_GRAPH_SPEC_TO_JSON_FIELDS(b, spec);
  json_builder_set_member_name(b, "linkReport");
  gchar *runtime_link_plan = d_pipeline_link_plan_json(spec ? spec->d_pipeline_json : "{}");
  add_json_value(b, runtime_link_plan, "{\"ok\":true,\"errors\":[],\"warnings\":[],\"links\":[]}");
  g_free(runtime_link_plan);
  json_builder_set_member_name(b, "stages");
  json_builder_begin_array(b);
  if (spec) {
    for (guint i = 0; i < spec->stage_count; i++) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "id");
      json_builder_add_string_value(b, spec->stages[i].id);
      json_builder_set_member_name(b, "kind");
      json_builder_add_string_value(b, spec->stages[i].kind);
      json_builder_set_member_name(b, "plugin");
      json_builder_add_string_value(b, spec->stages[i].plugin);
      if (spec->stages[i].dims_json[0]) {
        json_builder_set_member_name(b, "dims");
        add_json_value(b, spec->stages[i].dims_json, "{}");
      }
      json_builder_set_member_name(b, "params");
      add_json_value(b, spec->stages[i].params_json, "{}");
      json_builder_set_member_name(b, "dsl");
      add_json_value(b, spec->stages[i].dsl_json, "{}");
      json_builder_end_object(b);
    }
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "audioStages");
  json_builder_begin_array(b);
  if (spec) {
    for (guint i = 0; i < spec->audio_stage_count; i++) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "id");
      json_builder_add_string_value(b, spec->audio_stages[i].id);
      json_builder_set_member_name(b, "kind");
      json_builder_add_string_value(b, spec->audio_stages[i].kind);
      json_builder_set_member_name(b, "plugin");
      json_builder_add_string_value(b, spec->audio_stages[i].plugin);
      if (spec->audio_stages[i].dims_json[0]) {
        json_builder_set_member_name(b, "dims");
        add_json_value(b, spec->audio_stages[i].dims_json, "{}");
      }
      json_builder_set_member_name(b, "params");
      add_json_value(b, spec->audio_stages[i].params_json, "{}");
      json_builder_set_member_name(b, "dsl");
      add_json_value(b, spec->audio_stages[i].dsl_json, "{}");
      json_builder_end_object(b);
    }
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *out = json_generator_to_data(g, NULL);
  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out;
}

gchar *graph_spec_summary_json(const GraphSpec *spec, const gchar *state, const gchar *error) {
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "state");
  json_builder_add_string_value(b, state ? state : "unknown");
  json_builder_set_member_name(b, "runtimeName");
  json_builder_add_string_value(b, spec ? spec->runtime_name : "");
  json_builder_set_member_name(b, "programName");
  json_builder_add_string_value(b, spec ? spec->program_name : "");
  json_builder_set_member_name(b, "runtimeParams");
  add_json_value(b, spec ? spec->runtime_params_json : "{}", "{}");
  json_builder_set_member_name(b, "sourceUri");
  json_builder_add_string_value(b, spec ? spec->source_uri : "");
  json_builder_set_member_name(b, "sinkUri");
  json_builder_add_string_value(b, spec ? spec->sink_uri : "");
  json_builder_set_member_name(b, "clockPolicy");
  add_json_value(b, spec ? spec->clock_policy_json : "{}", "{}");
  json_builder_set_member_name(b, "perfRingPath");
  json_builder_add_string_value(b, spec ? spec->perf_ring_path : "");
  json_builder_set_member_name(b, "output");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "codec");
  json_builder_add_string_value(b, "hevc");
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, spec ? spec->output_width : 0);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, spec ? spec->output_height : 0);
  json_builder_set_member_name(b, "fps");
  json_builder_add_int_value(b, spec ? spec->output_fps : 0);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "processing");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "width");
  json_builder_add_int_value(b, spec ? spec->processing_width : 0);
  json_builder_set_member_name(b, "height");
  json_builder_add_int_value(b, spec ? spec->processing_height : 0);
  json_builder_set_member_name(b, "dPipeline");
  add_json_value(b, spec ? spec->d_pipeline_json : "{}", "{}");
  json_builder_set_member_name(b, "linkReport");
  gchar *summary_link_plan = d_pipeline_link_plan_json(spec ? spec->d_pipeline_json : "{}");
  add_json_value(b, summary_link_plan, "{\"ok\":true,\"errors\":[],\"warnings\":[],\"links\":[]}");
  g_free(summary_link_plan);
  json_builder_end_object(b);
  json_builder_set_member_name(b, "stageCount");
  json_builder_add_int_value(b, spec ? spec->stage_count : 0);
  json_builder_set_member_name(b, "audioStageCount");
  json_builder_add_int_value(b, spec ? spec->audio_stage_count : 0);
  json_builder_set_member_name(b, "stages");
  json_builder_begin_array(b);
  if (spec) {
    for (guint i = 0; i < spec->stage_count; i++) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "id");
      json_builder_add_string_value(b, spec->stages[i].id);
      json_builder_set_member_name(b, "kind");
      json_builder_add_string_value(b, spec->stages[i].kind);
      json_builder_set_member_name(b, "plugin");
      json_builder_add_string_value(b, spec->stages[i].plugin);
      if (spec->stages[i].dims_json[0]) {
        json_builder_set_member_name(b, "dims");
        add_json_value(b, spec->stages[i].dims_json, "{}");
      }
      json_builder_set_member_name(b, "params");
      add_json_value(b, spec->stages[i].params_json, "{}");
      json_builder_set_member_name(b, "dsl");
      add_json_value(b, spec->stages[i].dsl_json, "{}");
      json_builder_end_object(b);
    }
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "audioStages");
  json_builder_begin_array(b);
  if (spec) {
    for (guint i = 0; i < spec->audio_stage_count; i++) {
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "id");
      json_builder_add_string_value(b, spec->audio_stages[i].id);
      json_builder_set_member_name(b, "kind");
      json_builder_add_string_value(b, spec->audio_stages[i].kind);
      json_builder_set_member_name(b, "plugin");
      json_builder_add_string_value(b, spec->audio_stages[i].plugin);
      if (spec->audio_stages[i].dims_json[0]) {
        json_builder_set_member_name(b, "dims");
        add_json_value(b, spec->audio_stages[i].dims_json, "{}");
      }
      json_builder_set_member_name(b, "params");
      add_json_value(b, spec->audio_stages[i].params_json, "{}");
      json_builder_set_member_name(b, "dsl");
      add_json_value(b, spec->audio_stages[i].dsl_json, "{}");
      json_builder_end_object(b);
    }
  }
  json_builder_end_array(b);
  if (error && *error) {
    json_builder_set_member_name(b, "error");
    json_builder_add_string_value(b, error);
  }
  json_builder_end_object(b);
  JsonGenerator *g = json_generator_new();
  JsonNode *root = json_builder_get_root(b);
  json_generator_set_root(g, root);
  gchar *out = json_generator_to_data(g, NULL);
  json_node_free(root);
  g_object_unref(g);
  g_object_unref(b);
  return out;
}
