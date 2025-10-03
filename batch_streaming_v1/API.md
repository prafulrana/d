# Control API v1

A tiny, opinionated HTTP API to request per‑stream outputs. Simple contract: your orchestrator gives us an RTSP URL to pull; we return a `streamId` and an output `rtspUrl` you can hand to your viewers.

## Base
- URL: `http://<HOST>:<CTRL_PORT>` (default `:8080`)
- Content type: requests `application/json`; responses `application/json`
- Auth: none (run on trusted hosts only)

## Concepts
- A stream has a stable numeric `streamId` (aka slot/index). Mapping is deterministic:
  - RTSP path: `/s<streamId>` (e.g., `/s0`)
  - UDP port: `BASE_UDP_PORT + streamId` (default base `5000`)
  - Demux pad: `src_<streamId>`
- Encoder policy (hard limits):
  - `streamId < HW_THRESHOLD` → NVENC (HW)
  - `streamId >= HW_THRESHOLD` → software H.264 (`x264enc`, fallback `avenc_h264`, `openh264enc`)
  - If NVENC is not available, `HW_THRESHOLD` is treated as `0` (no HW slots) and requests beyond SW capacity return 429
- Pacing: sources flagged live and sinks use the clock; outputs are realtime paced (no > 30 fps bursts)
- Inference: runs every frame (PGIE `interval=0`)

## Endpoints (v1)

### POST `/requestStream`
Request a new output stream by providing an RTSP URL to pull.

- Request body
  - `url` (required string): upstream RTSP URL to pull (from your MediaMTX)

- Response 200
```
{
  "streamId": 7,
  "ingest": "rtsp://mtx.example:8554/cam1",
  "rtspUrl": "rtsp://<PUBLIC_HOST>:<RTSP_PORT>/s7",
  "path": "/s7",
  "udp": 5007,
  "encoder": "nvenc" | "x264" | "avenc" | "openh264"
}
```

- Errors
  - 400 `{ "error": "invalid_param" }` (missing/invalid url)
  - 429 `{ "error": "capacity_exceeded", "max": <int>, "hw_max": <int>, "sw_max": <int> }`
  - 500 `{ "error": "internal_error" }`

- Notes
  - Allocates the lowest free `streamId` and builds the per‑stream branch
  - RTSP path and UDP port follow the stable mapping described above

- Example
```
curl -sS -X POST \
  -H 'Content-Type: application/json' \
  -d '{"url":"rtsp://mtx.example:8554/cam1"}' \
  http://localhost:8080/requestStream
```

### GET `/streams`
List capacity and current streams.

- Response 200
```
{
  "max": 64,
  "hw_max": 8,
  "sw_max": 56,
  "streams": [
    {
      "id": 0,
      "path": "/s0",
      "rtspUrl": "rtsp://<PUBLIC_HOST>:<RTSP_PORT>/s0",
      "udp": 5000,
      "encoder": "nvenc",
      "state": "ready"
    }
  ]
}
```

## Status Codes
- 200 OK — success
- 400 Bad Request — invalid or missing params
- 429 Too Many Requests — capacity exhausted (HW or total)
- 500 Internal Server Error — unexpected failure

## Environment Variables
- `CTRL_PORT` — HTTP control port (default 8080)
- `RTSP_PORT` — RTSP TCP port (default 8554)
- `BASE_UDP_PORT` — starting UDP port for RTP egress (default 5000)
- `PUBLIC_HOST` — host/IP returned in URLs
- `SAMPLE_URI` — default source if `url` is omitted in `requestStream`
- `HW_THRESHOLD` — number of NVENC streams (default 8)
- `SW_MAX` — number of software streams (default 56)
- `MAX_STREAMS` — total streams (default `HW_THRESHOLD + SW_MAX`)
- `X264_THREADS` — threads per x264 encoder (run.sh defaults to 2)
- `IDLE_TTL_SECS` — idle deletion timeout (default 60)

## Behavior Guarantees
- Deterministic mapping of `streamId` → RTSP path, UDP port, and demux pad
- Realtime pacing: sources are live; outputs use sink clock (no >30 fps bursts)
- Inference runs every frame
- Strict cleanup on delete: pads released, elements set to NULL and removed, memory freed

## Compatibility & Notes
- DeepStream 8.0; pre‑demux is built in code: `nvmultiurisrcbin (live-source=1, batched-push-timeout≈33ms) → nvinfer(pgie.txt) → nvstreamdemux`
- RTSP wrapping pattern: `udpsrc (H264 RTP caps) → rtph264depay → rtph264pay name=pay0`
- Security: binds to 0.0.0.0; no auth. Use firewalling or run on trusted networks
