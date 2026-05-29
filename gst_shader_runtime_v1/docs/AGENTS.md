# D Runtime API Agent Canon

This is the local API and graph contract guide for agents working inside the D
runtime docs tree. Do not add sibling markdown files; extend this file or move
hard constraints into generated schema/tests.

D is a generic GPU video processing runtime. A caller sends D a graph, D opens
the source, normalizes the decoded frame stream, runs CUDA/TRT/Maxine stages,
and publishes encoded HEVC/AAC packets to the requested sink.

D does not own provider login, catalogs, users, UI, CDN policy, or product
state. Those belong to the control plane that calls this API.

## Runtime Model

```text
control plane
  -> D control API
  -> graph JSON
  -> source receiver
  -> normalized decoded frame timeline
  -> graph stages
  -> HEVC/AAC sink
```

The source receiver is the quarantine boundary. Upstream timestamps, segment
gaps, packet reorder, and discontinuities are treated as evidence. The graph
receives a monotonic frame timeline according to `clockPolicy`.

## Build And Run

The native runtime must be built inside the Docker/DeepStream/CUDA image. Do not
compile D C/CUDA code with host-side `make`, `gcc`, or `nvcc`.

Standalone D build:

```bash
docker build -t d-gst-runtime:latest .
```

When D is consumed from 99KS, use the consumer lifecycle wrapper:

```bash
npm run d:build
npm run d:run
npm run d:deploy
npm run d:restart
npm run d:status
```

The container listens on `CTRL_PORT`, default `8088`.

## Endpoints

### `GET /v1/status`

Returns the runtime state.

Example:

```bash
curl -fsS http://127.0.0.1:8088/v1/status
```

Response shape:

```json
{
  "state": "running",
  "runtimeName": "d-main",
  "programName": "default",
  "sourceUri": "https://example.invalid/live/index.m3u8",
  "sinkUri": "unix:/run/d/output.ts.sock",
  "clockPolicy": {
    "kind": "live",
    "videoClockMode": "source-pts",
    "audioPacingMode": "video-gated",
    "maxAudioLeadMs": 750,
    "maxAvDeltaMs": 250
  },
  "output": {
    "codec": "hevc",
    "width": 3840,
    "height": 2160,
    "fps": 60
  },
  "processing": {
    "width": 1920,
    "height": 1080,
    "dPipeline": {}
  },
  "stages": [],
  "audioStages": []
}
```

`GET /status` is an alias for local health checks.

### `POST /v1/programs/:program/select`

Starts or replaces a program. The request body is a graph matching
`graphs/d-pipeline.schema.json`.

Example:

```bash
curl -fsS -X POST http://127.0.0.1:8088/v1/programs/default/select \
  -H 'Content-Type: application/json' \
  --data @graphs/default_video.json
```

Response:

```json
{
  "ok": true,
  "state": "running",
  "programName": "default",
  "output": {
    "codec": "hevc",
    "width": 3840,
    "height": 2160,
    "fps": 60
  }
}
```

Selection is authoritative. D stops the previous native program before starting
the new graph for that runtime.

### `POST /v1/programs/:program/stop`

Stops the selected program and leaves D idle.

Example:

```bash
curl -fsS -X POST http://127.0.0.1:8088/v1/programs/default/stop
```

Response:

```json
{
  "ok": true,
  "state": "idle",
  "programName": "default"
}
```

## Graph Envelope

Every graph has the same envelope, whether the source is live, file, VOD, a
camera, or a future sports-scoring feed.

Required or normally-present fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `$schema` | string | Schema reference, usually `./d-pipeline.schema.json`. |
| `runtimeName` | string | Runtime instance name for status/evidence. |
| `programName` | string | Program lane name. |
| `sourceUri` | string | libavformat-openable source URI. |
| `sourceHeaders` | string | Optional CRLF/newline header block. |
| `isLive` | boolean | Live/open-ended source vs bounded source. |
| `executionMode` | string | `live` or `file`. |
| `clockPolicy` | object | Timeline and A/V policy. |
| `outputWidth` | number | Encoded output width. |
| `outputHeight` | number | Encoded output height. |
| `outputFps` | number | Encoded output cadence. |
| `processingWidth` | number | Selected graph/model input width. |
| `processingHeight` | number | Selected graph/model input height. |
| `bitrateBps` | number | HEVC target bitrate. |
| `maxBitrateBps` | number | HEVC max bitrate. |
| `perfRingPath` | string | Optional native perf ring path. |
| `sinkUri` | string | Encoded output sink. |
| `dPipeline` | object | Canonical stage/link/caps/timing contract. |
| `stages` | array | Runtime stage projection for evidence/status. |
| `audioStages` | array | Audio branch projection. |

Minimal shape:

```json
{
  "$schema": "./d-pipeline.schema.json",
  "runtimeName": "d-main",
  "programName": "default",
  "sourceUri": "https://example.invalid/live/index.m3u8",
  "sourceHeaders": "",
  "isLive": true,
  "executionMode": "live",
  "clockPolicy": {
    "kind": "live",
    "videoClockMode": "source-pts",
    "audioPacingMode": "video-gated",
    "maxAudioLeadMs": 750,
    "maxAvDeltaMs": 250
  },
  "outputWidth": 3840,
  "outputHeight": 2160,
  "outputFps": 60,
  "processingWidth": 1920,
  "processingHeight": 1080,
  "bitrateBps": 24000000,
  "maxBitrateBps": 32000000,
  "sinkUri": "unix:/run/d/default.ts.sock",
  "dPipeline": {},
  "stages": [],
  "audioStages": []
}
```

A production graph must include a full `dPipeline`; the empty object above is
only to show envelope placement.

## Clock Policy

`clockPolicy` tells the source receiver and graph how to convert upstream media
into a downstream frame timeline.

```json
{
  "kind": "live",
  "videoClockMode": "source-pts",
  "audioPacingMode": "video-gated",
  "maxAudioLeadMs": 750,
  "maxAvDeltaMs": 250
}
```

Modes:

| Field | Values | Meaning |
| --- | --- | --- |
| `kind` | `live`, `vod` | Open-ended live vs bounded media. |
| `videoClockMode` | `source-pts`, `sasta-pts-gap-squash` | Source timestamp handling. Provider-specific names may appear only as policy values emitted by a control plane. |
| `audioPacingMode` | `video-gated`, `source-pts` | Whether audio is paced against normalized video or source PTS. |

For live sources with bad upstream PTS, use a policy that tells D to normalize
the source receiver timeline. D will log upstream PTS jumps/jitter as evidence
while downstream stages receive a monotonic decoded-frame clock.

For file/VOD, use:

```json
{
  "kind": "vod",
  "videoClockMode": "source-pts",
  "audioPacingMode": "source-pts",
  "maxAudioLeadMs": 0,
  "maxAvDeltaMs": 250
}
```

## Canonical `dPipeline`

`dPipeline` is the runtime execution contract. It should include:

- `name`, `version`, and `executor`
- graph-level `caps`
- `clockPolicy`, `encoderPolicy`, and `devicePolicy`
- ordered `stages`
- generated/validated adjacent `links`
- `linkPlan`
- filters/shaders
- per-stage timing fields:
  - `inputFps`
  - `inferFps`
  - `outputFps`
  - `cadenceOwner`
  - `ptsPolicy`
  - `frameHoldPolicy`

Stage IDs are durable schema IDs. They are not UI labels.

Adjacent links must agree on caps. A bad link should block selection before D
launch instead of letting D invent a fallback graph.

## Source Receiver

The source receiver is intentionally upstream-hostile:

- opens any libavformat-supported source URI
- keeps source headers out of logs except redacted evidence
- decodes on GPU where supported
- normalizes live frame cadence before graph stages
- treats raw PTS as evidence, not as a trusted clock
- drops or reports non-monotonic source frames according to policy
- emits structured timing evidence

Typical evidence:

```text
source_receiver mode=normalize-live-decode-order source_fps=25.000 output_fps=60.000 pts_policy=upstream-evidence-only downstream_clock=monotonic
video clock source-pts-jitter-ignored raw_interval_s=0.060 used_interval_s=0.040
video clock sasta-pts-gap-squashed raw_interval_s=6.040 used_interval_s=0.040
timing_evidence frame_index=120 input_pts_us=... stage_pts_us=... output_pts=...
```

## Perf Ring

If `perfRingPath` is set, D writes native cumulative counters into a shared ring.
The control plane can read it without parsing stderr or touching media bytes.

Counters include:

- input/output frames
- encoded bytes
- video/audio timelines
- A/V delta
- per-stage cumulative CUDA/audio/encode time
- process counters

The ring path should be a tmpfs or shared runtime volume.

## JavaScript SDK

The reference helper lives at:

```text
sdk/js/d-runtime-client.mjs
```

Example:

```js
import { DRuntimeClient, buildLiveGraph } from './sdk/js/d-runtime-client.mjs';

const client = new DRuntimeClient({
  controlUrl: 'http://127.0.0.1:8088',
  programName: 'demo',
});

const graph = buildLiveGraph({
  sourceUri: 'https://example.invalid/live/index.m3u8',
  sinkUri: 'unix:/run/d/demo.ts.sock',
  dPipeline: buildYourCanonicalDPipeline(),
});

await client.select(graph);
console.log(await client.status());
```

The SDK does not resolve provider URLs and does not proxy media bytes.

## File Processing Example

```json
{
  "$schema": "./d-pipeline.schema.json",
  "runtimeName": "d-main",
  "programName": "file-upscale",
  "sourceUri": "file:///inputs/source.mkv",
  "isLive": false,
  "executionMode": "file",
  "clockPolicy": {
    "kind": "vod",
    "videoClockMode": "source-pts",
    "audioPacingMode": "source-pts",
    "maxAudioLeadMs": 0,
    "maxAvDeltaMs": 250
  },
  "sinkUri": "file:///outputs/source.hevc.ts",
  "outputUri": "file:///outputs/source.hevc.ts",
  "dPipeline": {}
}
```

## Sports Scoring / Overlay Example

A sports control plane can build a graph with inference and overlay stages while
keeping the same D runtime API:

```json
{
  "programName": "court-1-scoreboard",
  "sourceUri": "rtsp://camera.local/court-1",
  "isLive": true,
  "executionMode": "live",
  "clockPolicy": {
    "kind": "live",
    "videoClockMode": "source-pts",
    "audioPacingMode": "video-gated",
    "maxAudioLeadMs": 500,
    "maxAvDeltaMs": 250
  },
  "dPipeline": {
    "name": "sports-score-overlay",
    "stages": [
      {
        "id": "source_receiver",
        "kind": "source",
        "timing": {
          "cadenceOwner": "pass-cadence",
          "ptsPolicy": "preserve-source"
        }
      },
      {
        "id": "sam3_player_tracker",
        "kind": "model",
        "engine": "tensorrt"
      },
      {
        "id": "score_overlay",
        "kind": "overlay",
        "engine": "cuda"
      },
      {
        "id": "output_clock",
        "kind": "motion",
        "op": "cadence-adapter",
        "timing": {
          "cadenceOwner": "output-clock-owner",
          "ptsPolicy": "stage-output"
        }
      }
    ],
    "links": []
  }
}
```

The example is schematic. The exact stage definitions must match
`graphs/d-pipeline.schema.json`.

## Error Semantics

Common errors:

| Error | Meaning |
| --- | --- |
| `bad_json` | Request body was not a graph JSON object. |
| `select_failed` | D could not start the native graph. |
| `open_input` | Source could not be opened. |
| `pipeline_manifest` | Graph/stage manifest was invalid. |
| `worker_init` | Native GPU/model worker init failed. |
| `vsr_configure` | Upscaler/model setup failed. |
| `open_output` | Sink could not be opened. |

Errors are fail-fast. D should not silently publish a fallback graph, CPU
pipeline, or H.264 output when the requested graph fails.

## Non-Goals

- provider auth
- catalog/search
- UI
- player state
- CDN ownership
- VPN/home-IP policy
- CPU inference fallback
- Node/Python media-byte forwarding
- product-specific tile/channel concepts
