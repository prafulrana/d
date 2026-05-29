// JSON graph parsing. The control process converts this declarative graph into
// the native GPU processor request; frames stay in the native CUDA/NV codec path.
#ifndef DGST_GRAPH_H
#define DGST_GRAPH_H

#include <glib.h>

#define DGST_MAX_STAGES 128

typedef struct {
  gchar id[64];
  gchar kind[64];
  gchar plugin[64];
  gchar dims_json[512];
  gchar params_json[2048];
  gchar dsl_json[2048];
} GraphStage;

#include "generated/d_graph_contract.h"

typedef struct {
  D_GRAPH_SPEC_FIELDS
} GraphSpec;

void graph_spec_init(GraphSpec *spec);
gboolean graph_spec_load_file(const gchar *path, GraphSpec *spec, gchar **error_out);
gboolean graph_spec_parse_json(const gchar *json, GraphSpec *spec, gchar **error_out);
gboolean graph_spec_validate_links(const GraphSpec *spec, gchar **error_out);
gchar *graph_spec_to_runtime_json(const GraphSpec *spec);
gchar *graph_spec_summary_json(const GraphSpec *spec, const gchar *state, const gchar *error);

#endif
