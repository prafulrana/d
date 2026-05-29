#include "control.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include "app.h"
#include "graph.h"
#include "log.h"

static int create_listener(guint port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int opt = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(s);
    return -1;
  }
  if (listen(s, 16) != 0) {
    close(s);
    return -1;
  }
  return s;
}

static gchar *request_body(const char *buf) {
  const char *body = strstr(buf, "\r\n\r\n");
  return body ? g_strdup(body + 4) : g_strdup("");
}

static gssize request_content_length(const char *buf) {
  const char *headers_end = strstr(buf, "\r\n\r\n");
  if (!headers_end) return -1;
  const char *p = buf;
  while (p && p < headers_end) {
    const char *line_end = strstr(p, "\r\n");
    if (!line_end || line_end > headers_end) line_end = headers_end;
    if ((line_end - p) >= 15 && g_ascii_strncasecmp(p, "Content-Length:", 15) == 0) {
      const char *v = p + 15;
      while (v < line_end && g_ascii_isspace(*v)) v++;
      return (gssize)g_ascii_strtoll(v, NULL, 10);
    }
    p = line_end + 2;
  }
  return 0;
}

static gchar *recv_full_request(int c) {
  GString *req = g_string_sized_new(65536);
  gsize header_len = 0;
  gssize content_len = -1;
  const gsize max_request = 4 * 1024 * 1024;
  for (;;) {
    char chunk[8192];
    ssize_t n = recv(c, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    g_string_append_len(req, chunk, (gssize)n);
    if (req->len > max_request) break;
    if (header_len == 0) {
      const char *headers_end = strstr(req->str, "\r\n\r\n");
      if (headers_end) {
        header_len = (gsize)(headers_end - req->str) + 4;
        content_len = request_content_length(req->str);
        if (content_len < 0) content_len = 0;
      }
    }
    if (header_len > 0 && req->len >= header_len + (gsize)content_len) break;
  }
  return g_string_free(req, FALSE);
}

static void send_response(int c, int status, const char *content_type, const char *body) {
  const char *reason = status == 200 ? "OK" : (status == 400 ? "Bad Request" : "Internal Server Error");
  gchar *hdr = g_strdup_printf(
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
    status, reason, content_type, strlen(body));
  send(c, hdr, strlen(hdr), 0);
  send(c, body, strlen(body), 0);
  g_free(hdr);
}

static gboolean has_prefix(const char *buf, const char *prefix) {
  return g_str_has_prefix(buf, prefix);
}

static gchar *program_from_request(const char *buf, const char *action) {
  const char *prefix = "POST /v1/programs/";
  if (!g_str_has_prefix(buf, prefix)) return NULL;
  const char *program_start = buf + strlen(prefix);
  const char *slash = strchr(program_start, '/');
  if (!slash || slash == program_start) return NULL;
  const char *action_start = slash + 1;
  size_t action_len = strlen(action);
  if (strncmp(action_start, action, action_len) != 0) return NULL;
  if (action_start[action_len] != ' ' && action_start[action_len] != '?' && action_start[action_len] != '/') return NULL;
  gchar *encoded = g_strndup(program_start, (gsize)(slash - program_start));
  gchar *decoded = g_uri_unescape_string(encoded, NULL);
  if (decoded && *decoded) {
    g_free(encoded);
    return decoded;
  }
  g_free(decoded);
  return encoded;
}

static void handle_request(int c, const char *buf) {
  if (has_prefix(buf, "GET /v1/status ") || has_prefix(buf, "GET /status ")) {
    gchar *json = app_status_json();
    send_response(c, 200, "application/json", json);
    g_free(json);
    return;
  }
  gchar *stop_program = program_from_request(buf, "stop");
  if (stop_program) {
    app_stop_graph();
    gchar *escaped = g_strescape(stop_program, NULL);
    gchar *json = g_strdup_printf("{\"ok\":true,\"state\":\"idle\",\"programName\":\"%s\"}\n", escaped ? escaped : "");
    send_response(c, 200, "application/json", json);
    g_free(escaped);
    g_free(json);
    g_free(stop_program);
    return;
  }
  gchar *select_program = program_from_request(buf, "select");
  if (select_program) {
    gchar *body = request_body(buf);
    GraphSpec spec;
    gchar *error = NULL;
    if (!graph_spec_parse_json(body, &spec, &error)) {
      gchar *json = g_strdup_printf("{\"ok\":false,\"error\":\"%s\"}\n", error ? error : "bad_json");
      send_response(c, 400, "application/json", json);
      g_free(json);
      g_free(error);
      g_free(body);
      g_free(select_program);
      return;
    }
    g_strlcpy(spec.program_name, select_program, sizeof(spec.program_name));
    if (app_select_graph(&spec, &error)) {
      gchar *json = graph_spec_summary_json(&spec, "running", NULL);
      send_response(c, 200, "application/json", json);
      g_free(json);
    } else {
      gchar *json = g_strdup_printf("{\"ok\":false,\"error\":\"%s\"}\n", error ? error : "select_failed");
      send_response(c, 500, "application/json", json);
      g_free(json);
    }
    g_free(error);
    g_free(body);
    g_free(select_program);
    return;
  }
  send_response(c, 404, "text/plain", "Not Found\n");
}

gpointer control_http_thread(gpointer data) {
  guint port = GPOINTER_TO_UINT(data);
  int s = create_listener(port);
  if (s < 0) {
    LOG_ERR("control listener failed on port %u", port);
    return NULL;
  }
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    gchar *buf = recv_full_request(c);
    if (buf && *buf) handle_request(c, buf);
    g_free(buf);
    close(c);
  }
  return NULL;
}
