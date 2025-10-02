# Repository Structure

- Dockerfile — Builds the runtime image (DeepStream 8.0 base, compiles C server).
- build.sh — One‑shot Docker image build helper.
- run.sh — Runs the C server and passes env vars (`RTSP_PORT`, `BASE_UDP_PORT`, `USE_OSD`, `PUBLIC_HOST`).
  - Also mounts `./models` on host to `/models` in the container to persist the TensorRT engine across runs.
- README.md — Architecture overview and usage.
- plan.md — Current implementation plan and next steps.
- STANDARDS.md — How to run/test; code cleanliness expectations.
- pipeline.txt — Optional pre‑demux pipeline (nvmultiurisrcbin → nvinfer → nvstreamdemux name=demux). Post‑demux is built by C.
- pgie.txt — Primary GIE (nvinfer) configuration.
- src/rtsp_server.c — Minimal C app (production path):
  - Parses/launches `pipeline.txt`.
  - Builds post‑demux branches and links `nvstreamdemux` request pads.
  - Per‑stream branch: queue (leaky downstream, 200ms limit) → nvvideoconvert → caps video/x-raw(memory:NVMM),format=RGBA → (nvosdbin) → nvvideoconvert → caps video/x-raw(memory:NVMM),format=NV12 → nvv4l2h264enc → h264parse → rtph264pay → udpsink(127.0.0.1:BASE_UDP_PORT+N)
  - Starts GstRtspServer and mounts endpoints:
    - `/test` — synthetic videotestsrc for sanity checks (always available at start).
    - `/sN` — mounted on demand when a stream is added via the control API; wraps from UDP (udpsrc port=<p> buffer-size=524288 name=pay0, H264 RTP).
  - Env vars:
    - `RTSP_PORT` (default 8554), `BASE_UDP_PORT` (default 5000), `USE_OSD` (default 1), `PUBLIC_HOST` (used in returned RTSP URLs).
  - Control API (single happy path):
    - `GET /add_demo_stream` — Adds one demo source (DeepStream sample 1080p) via nvmultiurisrcbin REST and builds a new per‑stream branch. Returns JSON: `{ "path": "/sN", "url": "rtsp://<PUBLIC_HOST>:<rtsp_port>/sN" }`. Returns HTTP 429 with JSON error if capacity (64) is exceeded.

- Python (dev only, branch `python-try`):
  - Files: `Dockerfile.python`, `run_py.sh`, `src/server_py.py`.
  - Functionally similar to C (control API on `CONTROL_PORT` default 8081), but on this host hits NVENC session limits around ~8–10. Kept for readability and iteration — use C for 64‑stream scale.
- deepstream-8.0/ — Vendor assets and helper scripts (not modified by this app).
