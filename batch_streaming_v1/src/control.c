// Tiny control HTTP server (L2)
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include "log.h"
#include "state.h"
#include "branch.h"

// Minimal HTTP POST helper to nvmultiurisrcbin REST (localhost:9010/9000)
static gboolean http_post_localhost_port(const char *port_str, const char *path, const char *json, size_t json_len) {
  struct addrinfo hints; memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = NULL;
  int err = getaddrinfo("127.0.0.1", port_str, &hints, &res);
  if (err != 0 || !res) { LOG_WRN("REST: getaddrinfo failed: %s", gai_strerror(err)); if (res) freeaddrinfo(res); return FALSE; }
  int s = -1; for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
    s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s == -1) continue;
    if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(s); s = -1;
  }
  freeaddrinfo(res);
  if (s == -1) { LOG_WRN("REST: connect failed"); return FALSE; }
  gchar *req = g_strdup_printf(
    "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
    path, port_str, json_len);
  ssize_t n = send(s, req, strlen(req), 0); g_free(req);
  if (n < 0) { LOG_WRN("REST: send header failed"); close(s); return FALSE; }
  if (json_len > 0) { ssize_t m = send(s, json, json_len, 0); if (m < 0) { LOG_WRN("REST: send body failed"); close(s); return FALSE; } }
  char buf[256]; (void)recv(s, buf, sizeof(buf), 0); close(s);
  return TRUE;
}

static gboolean http_post_localhost_try(const char *path, const char *json, size_t json_len) {
  if (http_post_localhost_port("9010", path, json, json_len)) return TRUE;
  if (http_post_localhost_port("9000", path, json, json_len)) return TRUE;
  LOG_WRN("REST: failed to post on 9010 and 9000");
  return FALSE;
}

static int create_ctrl_listener(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { LOG_ERR("CTRL: socket create failed"); return -1; }
  int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const gchar *env = g_getenv("CTRL_PORT");
  if (env && *env) {
    guint port = (guint) g_ascii_strtoull(env, NULL, 10);
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) { g_ctrl_port = port; }
    else { LOG_WRN("CTRL: bind %u from env failed; falling back to 8080-8089", port); }
  }
  if (g_ctrl_port == 0) {
    for (int port = 8080; port < 8090; ++port) {
      addr.sin_port = htons((uint16_t)port);
      if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) { g_ctrl_port = (guint)port; break; }
    }
  }
  if (g_ctrl_port == 0) { LOG_ERR("CTRL: bind 8080-8089 failed"); close(s); return -1; }
  if (listen(s, 16) != 0) { LOG_ERR("CTRL: listen failed"); close(s); return -1; }
  LOG_INF("Control API listening on http://0.0.0.0:%u", g_ctrl_port);
  return s;
}

static const char* find_query_param(const char *req, const char *key) {
  // Very small helper: look for "?key=" or "&key=" and return pointer to value (not decoded)
  const char *q = strchr(req, ' '); // after method
  if (!q) return NULL;
  const char *path = q + 1; // begins with '/'
  const char *qm = strchr(path, '?');
  if (!qm) return NULL;
  const char *p = qm + 1;
  size_t klen = strlen(key);
  while (*p && *p != ' ') {
    if ((p[0] == '&' && strncmp(p+1, key, klen) == 0 && p[1+klen] == '=') ||
        (p == qm + 1 && strncmp(p, key, klen) == 0 && p[klen] == '=')) {
      const char *val = p + ((p[0] == '&') ? 2 + klen : 1 + klen);
      return val; // caller should copy until '&' or space
    }
    const char *amp = strchr(p, '&');
    const char *sp = strchr(p, ' ');
    if (!amp || (sp && sp < amp)) break;
    p = amp;
  }
  return NULL;
}

static void send_json(int c, const char *json) {
  gchar *hdr = g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(json));
  send(c, hdr, strlen(hdr), 0); send(c, json, strlen(json), 0);
  g_free(hdr);
}

static ssize_t read_more_body(int c, char *first_buf, ssize_t first_len, char **out_body, size_t *out_len) {
  // Parse Content-Length and collect the full body into a newly allocated buffer.
  const char *cl = strcasestr(first_buf, "Content-Length:");
  const char *hdr_end = strstr(first_buf, "\r\n\r\n");
  if (!hdr_end) hdr_end = strstr(first_buf, "\n\n");
  size_t want = 0;
  if (cl) {
    want = (size_t) strtoul(cl + strlen("Content-Length:"), NULL, 10);
  }
  if (!hdr_end || want == 0) { *out_body = NULL; *out_len = 0; return first_len; }
  const char *b0 = hdr_end + ((hdr_end[1] == '\r') ? 4 : 2);
  size_t have = (size_t)(first_len - (b0 - first_buf));
  if (have >= want) {
    *out_body = g_strndup(b0, want);
    *out_len = want; return first_len;
  }
  char *body = g_malloc0(want + 1);
  memcpy(body, b0, have);
  size_t need = want - have;
  while (need > 0) {
    ssize_t n = recv(c, body + (want - need), need, 0);
    if (n <= 0) break;
    need -= (size_t)n;
  }
  *out_body = body; *out_len = want - need;
  return first_len;
}

static gchar* extract_url_from_json(const char *body, size_t len) {
  if (!body || len == 0) return NULL;
  const char *k = "\"url\"";
  const char *p = body;
  while ((p = strstr(p, k)) != NULL) {
    const char *q = strchr(p + strlen(k), '"');
    if (!q) { p += strlen(k); continue; }
    // find next quote after colon
    const char *colon = strchr(p + strlen(k), ':'); if (!colon) { p += strlen(k); continue; }
    const char *quote = strchr(colon, '"'); if (!quote) { p += strlen(k); continue; }
    const char *end = quote + 1; while (end < body + len && *end != '"') end++;
    if (end >= body + len) break;
    return g_strndup(quote + 1, (gsize)(end - (quote + 1)));
  }
  return NULL;
}

static void handle_request(int c, const char *buf) {
  gboolean is_get_req = (g_str_has_prefix(buf, "GET /requestStream ") || g_str_has_prefix(buf, "GET /requestStream?"));
  gboolean is_post_req = g_str_has_prefix(buf, "POST /requestStream ");
  gboolean is_status = (g_str_has_prefix(buf, "GET /streams ") || g_str_has_prefix(buf, "GET /streams?") || g_str_has_prefix(buf, "GET /status "));
  if (!is_get_req && !is_post_req && !is_status) {
    const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
    send(c, resp, strlen(resp), 0);
    return;
  }
  if (is_status) {
    GString *j = g_string_new(NULL);
    g_string_append_printf(j, "{\n  \"max\": %u,\n  \"streams\": [\n", g_max_streams);
    gboolean first = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(g_streams); ++i) {
      if (!g_streams[i].in_use) continue;
      if (!first) g_string_append(j, ",\n");
      first = FALSE;
      g_string_append_printf(j,
        "    { \"index\": %u, \"path\": \"%s\", \"udp\": %u, \"encoder\": \"%s\" }",
        i, g_streams[i].path, g_streams[i].udp_port, g_streams[i].enc_kind[0] ? g_streams[i].enc_kind : "unknown");
    }
    g_string_append(j, "\n  ]\n}\n");
    gchar *hdr = g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", j->len);
    send(c, hdr, strlen(hdr), 0); send(c, j->str, j->len, 0);
    g_free(hdr); g_string_free(j, TRUE);
    return;
  }

  // Add stream (POST /requestStream with JSON body {"url":...} or GET /requestStream?url=...)
  guint index;
  g_mutex_lock(&g_state_lock);
  if (g_next_index >= g_max_streams) {
    g_mutex_unlock(&g_state_lock);
    gchar *json = g_strdup_printf("{\n  \"error\": \"capacity_exceeded\",\n  \"max\": %u\n}\n", g_max_streams);
    gchar *hdr = g_strdup_printf("HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(json));
    send(c, hdr, strlen(hdr), 0); send(c, json, strlen(json), 0);
    g_free(hdr); g_free(json);
    return;
  }
  index = g_next_index++;
  g_mutex_unlock(&g_state_lock);

  gchar *path = NULL; gchar *url = NULL;
  gboolean ok = add_branch_and_mount(index, &path, &url);
  if (ok) {
    // Resolve source URI from POST body or GET ?url=...
    gchar *src_uri = NULL;
    if (is_post_req) {
      char *body = NULL; size_t blen = 0; (void)read_more_body(c, (char*)buf, strlen(buf), &body, &blen);
      src_uri = extract_url_from_json(body ? body : "", blen);
      if (body) g_free(body);
    }
    if (!src_uri) {
      const char *v = find_query_param(buf, "url");
      if (v) { const char *end = v; while (*end && *end!=' '& *end!='\r' && *end!='\n' && *end!='&') end++; src_uri = g_strndup(v, (gsize)(end - v)); }
    }
    if (!src_uri) { // final fallback
      const gchar *env_sample = g_getenv("SAMPLE_URI");
      src_uri = g_strdup(env_sample ? env_sample : "file:///opt/nvidia/deepstream/deepstream/samples/streams/sample_1080p_h264.mp4");
    }
    gchar *body = g_strdup_printf(
      "{\n  \"key\": \"sensor\",\n  \"value\": {\n    \"camera_id\": \"api_%u\",\n    \"camera_url\": \"%s\",\n    \"change\": \"camera_add\"\n  },\n  \"headers\": { \"source\": \"app\" }\n}\n",
      index, src_uri);
    (void)http_post_localhost_try("/api/v1/stream/add", body, strlen(body));
    g_free(body);
    gchar *json = g_strdup_printf(
      "{\n  \"streamId\": %u,\n  \"ingest\": \"%s\",\n  \"rtspUrl\": \"%s\",\n  \"path\": \"%s\",\n  \"udp\": %u,\n  \"encoder\": \"%s\"\n}\n",
      index,
      src_uri,
      url,
      path,
      g_streams[index].udp_port,
      g_streams[index].enc_kind[0] ? g_streams[index].enc_kind : "unknown");
    send_json(c, json);
    g_free(json);
    g_free(src_uri);
  } else {
    const char *resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: close\r\n\r\nError\n";
    send(c, resp, strlen(resp), 0);
  }
  g_free(path); g_free(url);
}

gpointer control_http_thread(gpointer data) {
  (void)data;
  int s = create_ctrl_listener();
  if (s < 0) return NULL;
  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) continue;
    char buf[1024]; ssize_t n = recv(c, buf, sizeof(buf)-1, 0);
    if (n <= 0) { close(c); continue; }
    buf[n] = '\0';
    handle_request(c, buf);
    close(c);
  }
  return NULL;
}
