# Plan

Goal: Minimal C, single config. Post‑demux work stays simple; C only boots the pipeline and exposes RTSP mounts that are easy to test from macOS.

Pre‑change rule
- Always read samples in `deepstream-8.0/` before modifying code/docs. Confirmed our RTSP approach matches `deepstream_sink_bin.c` (UDP‑wrap with `rtph264pay` → `udpsink`, RTSP `udpsrc name=pay0`).

Current status
- /test works (software JPEG RTP).
- Switched post‑demux to DeepStream sample pattern: per‑stream encode + RTP/UDP egress; RTSP factories wrap from UDP (no intervideo).

Open items (execution plan)
1) Verify /s0..s2 playback via UDP‑wrapped RTSP
   - Branch: queue → (nvosdbin) → nvvideoconvert → NVMM NV12 → nvv4l2h264enc → h264parse → rtph264pay → udpsink:127.0.0.1:(5000+i)
   - RTSP: `( udpsrc address=127.0.0.1 port=5000+i caps="application/x-rtp, media=video, clock-rate=90000, encoding-name=H264, payload=96" name=pay0 )`
   - Mirrors DeepStream `deepstream_sink_bin.c` approach.
2) Keep C tiny and readable
   - Parse only `pipeline.txt`, build/link branches, start RTSP.
   - Minimal envs: `STREAMS`, `RTSP_PORT`.
3) Mac testability
   - Validate with ffplay over TCP from macOS; `/s0..s2` must play.

Notes
- We intentionally avoid Python. All serving is C + config.
- If DeepStream REST (port 9000) is occupied in your environment, it does not affect RTSP; warnings are benign.

---

Next Phase: Scale to 64 (readiness checklist)

Scope: Keep STREAMS=2 for now. Do not implement yet; capture changes to apply before scaling tests.

1) RTSP parity with DeepStream samples
- Add `udpsrc buffer-size` (e.g., 524288) in RTSP factory launch; drop `address=127.0.0.1` for exact parity with `deepstream_sink_bin.c`.

2) Per-branch queue tuning
- Set `queue leaky=2` (downstream) and conservative limits (e.g., `max-size-time=200000000`, or buffers=0/bytes=0) to bound latency under load.

3) Default OSD off for scale
- Flip default `USE_OSD=0` and keep env override. Avoid extra GPU work across many streams.

4) NVENC tuning and bitrate
- Keep `insert-sps-pps=1`, `idrinterval/iframeinterval` aligned to framerate. Consider lower per-stream bitrate (e.g., 2–3 Mbps @720p30) with a single env for consistency.

5) UDP ports configurability
- Add `BASE_UDP_PORT` env; ensure contiguous range `[BASE_UDP_PORT .. +STREAMS-1]` is free and documented.

6) Batch and pre-demux alignment
- Require `pipeline.txt max-batch-size == STREAMS`; confirm framerate/resize are set pre-demux to keep NVENC input uniform.

7) Logging and observability
- Keep branch link logs. Optionally add lightweight per-branch FPS counters (info-level only) for soak testing.

8) Capacity checks
- Document GPU encoder session capacity and expected aggregate bandwidth (e.g., 64 × 3 Mbps ≈ 192 Mbps) in STANDARDS.md before scaling.

9) Optional codec path
- Consider H265 path parity for bandwidth reduction (doc-first; no toggles until stable).
