#ifndef DGST_APP_H
#define DGST_APP_H

#include <glib.h>
#include "config.h"
#include "graph.h"

gboolean app_setup(const AppConfig *cfg);
void app_loop(void);
void app_teardown(void);
gboolean app_select_graph(const GraphSpec *graph, gchar **error_out);
void app_stop_graph(void);
gchar *app_status_json(void);

#endif
