// Runtime configuration read from environment.
#ifndef DGST_CONFIG_H
#define DGST_CONFIG_H

#include <glib.h>

typedef struct {
  guint ctrl_port;
  gchar *default_graph;
  gchar *mediamtx_rtsp_url;
  gchar *public_playback_url;
} AppConfig;

gboolean parse_args(int argc, char **argv, AppConfig *cfg);
void cleanup_config(AppConfig *cfg);

#endif
