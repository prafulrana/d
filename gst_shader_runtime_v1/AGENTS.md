# D Runtime Agent Canon

D is a reusable GPU video processing runtime. It is not a Sasta, Kodi, TV, or
99KS-specific media app. Consumers provide source URLs, headers, graph JSON,
runtime policy, and sinks; D provides the provider-neutral source receiver,
timeline normalization, CUDA/TRT/Maxine processing graph, encoded output, status,
and evidence.

Treat static files and VOD as first-class initial inputs; live streams are a
source mode, not the runtime's default mental model. Keep D generic enough for
files, VOD, sports scoring, overlays, detectors, health monitors, live streams,
and future model chains. Use names like
source receiver, decoded frame stream, graph stage, inference stage, overlay,
scoring, sink, clock policy, and evidence. Do not bake provider names or TV-only
assumptions into native architecture. Provider quirks belong in the control
plane graph/policy that launches D.

## Markdown Policy

This directory is agent-canon oriented. Do not add README, STANDARDS,
MIGRATION, plan, or architecture markdown files. Put runtime-wide rules here,
API/graph/user-facing contract notes in `docs/AGENTS.md`, SDK-specific rules in
`sdk/js/AGENTS.md`, and model artifact rules in `models/AGENTS.md`.

## Build/Run Lifecycle

D native code must be built inside the D Docker/DeepStream/CUDA image. Host-side
`make`, `gcc`, or `nvcc` is not canonical because developer hosts may not have
the CUDA, DeepStream, TensorRT, Maxine, or driver headers that the image has.

From the 99KS consumer repo, use:

```bash
npm run d:build
npm run d:run
npm run d:deploy
npm run d:restart
npm run d:status
```

Those commands call `99KS/scripts/d-runtime.mjs`, which routes through Docker
Compose service `d-gst-runtime`.

From this repo directly, the canonical standalone build is:

```bash
docker build -t d-gst-runtime:latest .
```

Do not add alternate host-native build paths unless they reproduce the exact D
runtime SDK image contract.

## Runtime Contract

- Graph JSON is the execution contract.
- Source receiver code quarantines upstream transport irregularities before
  graph stages see frames.
- Raw upstream timestamps are evidence. The downstream graph receives a
  normalized monotonic frame timeline according to declared clock policy.
- GPU residency is required between decode and encode.
- CPU inference, host pixel copies, H.264 fallback, and hidden provider-specific
  branches are not valid steady-state runtime paths.
