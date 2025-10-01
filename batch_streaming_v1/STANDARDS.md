# Standards

## Always Read DeepStream Samples First
- Before making any code or doc changes, read the samples under `deepstream-8.0/` in this repo. Align implementation and configuration with DeepStream 8.0 patterns, especially:
  - `sources/apps/apps-common/src/deepstream_sink_bin.c` (RTSP via UDP-wrap, `rtph264pay` + `udpsink`, `udpsrc name=pay0`)
  - `sources/apps/sample_apps/deepstream-app` (demux/sink wiring and config parsing)
  - `samples/configs/deepstream-app*` (`rtsp-port`, sink settings, demux usage)
- Do not introduce new approaches that diverge from these patterns unless documented rationale is added here.

## How To Run (Single Happy Path)
- Requirements: Docker with NVIDIA runtime (`--gpus all`), access to nvcr.io.
- Build: `./build.sh`
- Start service EMPTY (only `/test` present): `./run.sh`
- Add one demo stream (control API): `curl http://localhost:8080/add_demo_stream`
  - Response (example): `{ "path": "/s0", "url": "rtsp://<host>:8554/s0" }`
- Play from macOS: `ffplay -rtsp_transport tcp rtsp://<host>:8554/s0`
- Sanity any time: `ffplay -rtsp_transport tcp rtsp://<host>:8554/test`

## Configuration Strategy
- Single config file: `pipeline.txt` defines all pre‑demux behavior and sources.
- C code only handles post‑demux and RTSP. No encoder toggles or branches.
 - Per‑stream encode uses `nvv4l2h264enc` (NVENC), then RTP/UDP egress to localhost; RTSP wraps from UDP (DeepStream pattern).
 - Important: `pipeline.txt` must be a single line (no comments or shell line continuations). It is parsed by `gst_parse_launch`, not a shell.
 - OSD overlays are enabled by default (`USE_OSD=1`), matching DeepStream samples. Disable only for scale testing.
 - Queue per branch is tuned for low latency: `leaky=2` (downstream) and `max-size-time=200ms`.
 - RTSP factories wrap UDP using `udpsrc port=<p> buffer-size=524288 name=pay0` with H264 RTP caps.
 - Control API: service starts with no `/sN`. Hitting `GET /add_demo_stream` auto‑adds a sample source and mounts the next `/sN`, returning its RTSP URL as JSON.
 - Optional REST wrapper: set `AUTO_ADD_SAMPLES=N` to add N sample sources at runtime via nvmultiurisrcbin REST (port 9010). For a zero‑source start, omit `uri-list` in `pipeline.txt` and ensure `max-batch-size >= N`.

## Minimal Env Vars
- `RTSP_PORT` — RTSP TCP port (default 8554; auto‑increments if busy)
- `BASE_UDP_PORT` — starting UDP port for per‑stream RTP egress (default 5000)
- `USE_OSD` — enable per‑stream overlays (default 1)
- `SAMPLE_URI` — demo URI used by `add_demo_stream` (default DS sample 1080p H.264)
- `PUBLIC_HOST` — host/IP to return in RTSP URLs (default 10.243.249.215 via run.sh; override as needed)

## Code Cleanliness
- Favor config strings (pipeline, encoder choices) over code. Keep C small.
- Do not mix unrelated concerns; post‑demux logic only.
- Prefer explicit pad requests and clear error logs for linking.
- Keep defaults sensible; make behavior tunable via envs.
- No Python in this app; avoid extra frameworks.

## Troubleshooting
- `/test` works but `/sN` returns 503:
  - Confirm logs show: `Linked demux src_N to UDP egress port 5000+N` and `RTSP mounted ... (udp-wrap H264 RTP @127.0.0.1:5000+N)`.
  - Ensure `pipeline.txt` `max-batch-size` >= number of added streams.
  - Rebuild and rerun if you changed `pipeline.txt`.
- Port conflicts:
  - RTSP retries 8554..+9. Use the logged port in your ffplay URL.
  - DeepStream’s REST (9000) may already be bound; it doesn’t affect RTSP.

## Scaling Considerations
- Match `STREAMS` with `max-batch-size` in `pipeline.txt`; set framerate/resize pre‑demux for uniform NVENC input.
- Bandwidth planning: e.g., 64× streams at ~3 Mbps ≈ 192 Mbps aggregate.
- Encoder settings: keep `insert-sps-pps=1` and IDR/I‑frame intervals aligned to framerate; default bitrate ~3 Mbps @ 720p30.

## Style
- C: consistent naming, early returns, minimal globals, no dead paths.
- One config until stable: avoid options creep until `/sN` are reliable.
- Patches: keep changes surgical; avoid unrelated edits.
- Docs: concise, task‑oriented, and aligned to actual code.
