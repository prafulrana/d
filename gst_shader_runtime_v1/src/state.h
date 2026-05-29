#ifndef DGST_STATE_H
#define DGST_STATE_H

#include <glib.h>
#include "graph.h"

typedef struct {
  GMutex lock;
  GPid native_pid;
  GraphSpec graph;
  gchar *last_error;
  gboolean running;
  gboolean native_running;
} RuntimeState;

extern RuntimeState g_state;

#endif
