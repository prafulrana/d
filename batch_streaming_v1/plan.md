# Plan

Goal: One happy path (C, config‑first). Start empty (only `/test`), add demo streams via control API that returns the RTSP URL. Python path is experimental/readability only.

Pre‑change rule
- Always read samples in `deepstream-8.0/` before modifying code/docs. Confirmed our RTSP approach matches `deepstream_sink_bin.c` (UDP‑wrap with `rtph264pay` → `udpsink`, RTSP `udpsrc name=pay0`).

Current status
- C path (master): Stable at scale (NVENC → RTP/UDP → RTSP wrap). Control API `GET /add_demo_stream` mounts `/sN` and returns RTSP URL. Engine caching on host (`./models`).
- Python path (branch `python-try`): Works, simpler to read, but on this GPU hits NVENC session limits (~8–10). Kept as dev/demo only.

Open items (execution plan)
1) Control API (C, single happy path)
   - Add tiny HTTP server (e.g., on `:8080`) inside `rtsp_server.c` with `GET /add_demo_stream`.
   - Handler flow:
     - Determine next index N; request `demux` pad `src_N`.
     - Build per‑stream branch and link to UDP egress at `BASE_UDP_PORT+N`.
     - Mount RTSP factory at `/sN` wrapping UDP (udpsrc name=pay0).
     - POST to nvmultiurisrcbin REST (port 9010) to add `SAMPLE_URI`.
     - Respond `200` with `{ "path": "/sN", "url": "rtsp://<PUBLIC_HOST>:<rtsp_port>/sN" }`. If N reaches 64, return HTTP 429 `{ "error": "capacity_exceeded", "max": 64 }`.
   - Startup behavior: set `STREAMS=0`; only `/test` is mounted.
2) Verify end‑to‑end (C)
   - Start empty; curl `/add_demo_stream`; receive JSON; play returned URL via ffplay.
3) Keep C tiny and readable
   - Single config file; explicit pad linking; narrow API surface.
   - Minimal envs: `RTSP_PORT`, `BASE_UDP_PORT`, `USE_OSD`, `SAMPLE_URI`, `PUBLIC_HOST` (no startup count; service always starts empty).
3) Engine caching — DONE
   - Persist engine to host via `/models` mount (run.sh mounts `./models` to `/models`).
   - PGIE config points `model-engine-file` to `/models/trafficcamnet_b64_gpu0_fp16.engine`.
4) Mac testability
   - Validate with ffplay over TCP from macOS; `/s0..s2` must play.

Notes
- Use C for production scale. Python is optional for readability.
- DeepStream REST (9000) is independent of RTSP; logs are informational.

Lessons learned (encoder limits)
- NVENC session limits vary by GPU/driver. On this host, Python path hit limits ~8–10 while C reached 64. The difference came from stricter timing/sync and RTSP factory settings in C; we applied parity in Python but the GPU still refused new sessions intermittently.
- Effective NVENC knobs (used in C; added in Python): `insert-sps-pps=1`, `idrinterval/iframeinterval` aligned to framerate, `maxperf-enable=1`, `preset-level=1`, `num-surface-bufs=16`, and staggering stream additions (~200 ms) to avoid simultaneous opens.
- If you still hit limits, fall back to the C path or test with a Quadro/Datacenter GPU.

---

 Next Phase: Scale to 64 (C path)

Scope: Keep STREAMS=2 for now. Do not implement yet; capture changes to apply before scaling tests.

1) RTSP parity with DeepStream samples — DONE
   - `udpsrc buffer-size` used; no `address` in RTSP factory launch.

2) Per-branch queue tuning — DONE
   - `queue leaky=2`, `max-size-time=200ms`, `max-size-buffers=0`, `max-size-bytes=0`.

3) Default OSD stays ON for correctness; consider disabling only for heavy scale soak tests.

4) NVENC tuning and bitrate
   - Keep `insert-sps-pps=1`, `idrinterval/iframeinterval` aligned to framerate. Consider lower per‑stream bitrate (e.g., 2–3 Mbps @720p30). For tight GPUs, add `maxperf-enable=1 preset-level=1 num-surface-bufs=16` and stagger adds.

5) UDP ports configurability
- DONE: `BASE_UDP_PORT` env present; ensure `[BASE_UDP_PORT .. +STREAMS-1]` is free.

6) Batch and pre-demux alignment — DONE
   - `pipeline.txt max-batch-size=64`; `pgie.txt batch-size=64` with b64 engine path. Zero‑source start maintained.

7) Logging and observability
- Keep branch link logs. Optionally add lightweight per-branch FPS counters (info-level only) for soak testing.

8) Capacity checks
- Document GPU encoder session capacity and expected aggregate bandwidth (e.g., 64 × 3 Mbps ≈ 192 Mbps) in STANDARDS.md before scaling.

9) Optional codec path
- Consider H265 path parity for bandwidth reduction (doc-first; no toggles until stable).
