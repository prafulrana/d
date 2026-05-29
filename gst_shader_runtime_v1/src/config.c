#include "config.h"

static gchar *env_dup(const gchar *name, const gchar *fallback) {
  const gchar *value = g_getenv(name);
  return g_strdup((value && *value) ? value : fallback);
}

gboolean parse_args(int argc, char **argv, AppConfig *cfg) {
  (void)argc;
  (void)argv;
  cfg->ctrl_port = 8088;
  const gchar *port = g_getenv("CTRL_PORT");
  if (port && *port) cfg->ctrl_port = (guint)g_ascii_strtoull(port, NULL, 10);
  cfg->default_graph = env_dup("DGST_DEFAULT_GRAPH", "/opt/dgst/graphs/default_video.json");
  cfg->mediamtx_rtsp_url = env_dup("DGST_MEDIAMTX_RTSP_URL", "unix:/run/99ks/99sk.ts.sock");
  cfg->public_playback_url = env_dup("DGST_PUBLIC_PLAYBACK_URL", "http://localhost:8888/default/index.m3u8");
  return TRUE;
}

void cleanup_config(AppConfig *cfg) {
  g_free(cfg->default_graph);
  g_free(cfg->mediamtx_rtsp_url);
  g_free(cfg->public_playback_url);
}
