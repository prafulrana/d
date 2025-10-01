# Repository Structure

- Dockerfile — Builds the runtime image (DeepStream 8.0 base, compiles C server).
- build.sh — One‑shot Docker image build helper.
- run.sh — Runs the container and passes env vars (STREAMS, URI_LIST, etc.).
- README.md — Architecture overview and usage.
- plan.md — Current implementation plan and next steps.
- STANDARDS.md — How to run/test; code cleanliness expectations.
- pipeline.txt — Optional pre‑demux pipeline (nvmultiurisrcbin → nvinfer → nvstreamdemux name=demux). Post‑demux is built by C.
- pgie.txt — Primary GIE (nvinfer) configuration.
- src/rtsp_server.c — Minimal C app:
  - Parses/launches `pipeline.txt`.
  - Builds post‑demux branches and links `nvstreamdemux` request pads.
  - Per‑stream branch: queue (leaky downstream, 200ms limit) → nvvideoconvert → caps video/x-raw(memory:NVMM),format=RGBA → (nvosdbin) → nvvideoconvert → caps video/x-raw(memory:NVMM),format=NV12 → nvv4l2h264enc → h264parse → rtph264pay → udpsink(127.0.0.1:BASE_UDP_PORT+N)
  - Starts GstRtspServer and mounts endpoints:
    - `/test` — synthetic videotestsrc for sanity checks.
    - `/s0..sN-1` — wraps from UDP (udpsrc port=<p> buffer-size=524288 name=pay0, H264 RTP) for mac‑friendly RTSP.
  - Env vars:
    - `STREAMS` (default 2), `RTSP_PORT` (default 8554), `BASE_UDP_PORT` (default 5000), `USE_OSD` (default 0).
- deepstream-8.0/ — Vendor assets and helper scripts (not modified by this app).
