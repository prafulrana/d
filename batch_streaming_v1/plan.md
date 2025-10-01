# Plan

Goal: Minimal C, single config. Single happy path: start empty (only `/test`), add demo streams via a tiny control API that returns the RTSP URL.

Pre‑change rule
- Always read samples in `deepstream-8.0/` before modifying code/docs. Confirmed our RTSP approach matches `deepstream_sink_bin.c` (UDP‑wrap with `rtph264pay` → `udpsink`, RTSP `udpsrc name=pay0`).

Current status
- /test works (software JPEG RTP).
- Post‑demux matches DeepStream sample pattern: per‑stream encode + RTP/UDP egress; RTSP factories wrap from UDP (no intervideo).
- OSD overlays enabled; correct order: convert → RGBA → OSD → convert → NV12 → NVENC.
- We post to nvmultiurisrcbin REST to add sources; next, we’ll wrap this behind a simple `GET /add_demo_stream` in our service.

Open items (execution plan)
1) Implement control API (single happy path)
   - Add tiny HTTP server (e.g., on `:8080`) inside `rtsp_server.c` with `GET /add_demo_stream`.
   - Handler flow:
     - Determine next index N; request `demux` pad `src_N`.
     - Build per‑stream branch and link to UDP egress at `BASE_UDP_PORT+N`.
     - Mount RTSP factory at `/sN` wrapping UDP (udpsrc name=pay0).
     - POST to nvmultiurisrcbin REST (port 9010) to add `SAMPLE_URI`.
     - Respond `200` with `{ "path": "/sN", "url": "rtsp://<PUBLIC_HOST>:<rtsp_port>/sN" }`.
   - Startup behavior: set `STREAMS=0`; only `/test` is mounted.
2) Verify end‑to‑end
   - Start empty; curl `/add_demo_stream`; receive JSON; play returned URL via ffplay.
3) Keep C tiny and readable
   - Single config file; explicit pad linking; narrow API surface.
   - Minimal envs: `RTSP_PORT`, `BASE_UDP_PORT`, `USE_OSD`, `SAMPLE_URI`, `PUBLIC_HOST` (no startup count; service always starts empty).
3) Mac testability
   - Validate with ffplay over TCP from macOS; `/s0..s2` must play.

Notes
- We intentionally avoid Python. All serving is C + config.
- If DeepStream REST (port 9000) is occupied in your environment, it does not affect RTSP; warnings are benign.

---

Next Phase: Scale to 64 (readiness checklist)

Scope: Keep STREAMS=2 for now. Do not implement yet; capture changes to apply before scaling tests.

1) RTSP parity with DeepStream samples
- DONE: `udpsrc buffer-size` used; no `address` in RTSP factory launch.

2) Per-branch queue tuning
- DONE: `queue leaky=2`, `max-size-time=200ms`, buffers/bytes unset (0).

3) Default OSD off for scale
- Consider flipping `USE_OSD=0` only when scaling soak tests; current default remains on for correctness.

4) NVENC tuning and bitrate
- Keep `insert-sps-pps=1`, `idrinterval/iframeinterval` aligned to framerate. Consider lower per-stream bitrate (e.g., 2–3 Mbps @720p30) with a single env for consistency.

5) UDP ports configurability
- DONE: `BASE_UDP_PORT` env present; ensure `[BASE_UDP_PORT .. +STREAMS-1]` is free.

6) Batch and pre-demux alignment
- Require `pipeline.txt max-batch-size == STREAMS`; confirm framerate/resize are set pre-demux to keep NVENC input uniform.
- For zero‑source start + auto‑add: leave out `uri-list` and set `max-batch-size` to your expected maximum to avoid editing later.

7) Logging and observability
- Keep branch link logs. Optionally add lightweight per-branch FPS counters (info-level only) for soak testing.

8) Capacity checks
- Document GPU encoder session capacity and expected aggregate bandwidth (e.g., 64 × 3 Mbps ≈ 192 Mbps) in STANDARDS.md before scaling.

9) Optional codec path
- Consider H265 path parity for bandwidth reduction (doc-first; no toggles until stable).
