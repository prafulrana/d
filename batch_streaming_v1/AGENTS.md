# batch_streaming_v1 Agent Canon

This is a DeepStream batch-inference RTSP service, separate from the active
`gst_shader_runtime_v1` D runtime. Keep this app generic and local to batch
multi-stream inference.

## Architecture

The runtime shape is:

```text
nvmultiurisrcbin
  -> nvinfer batch inference
  -> nvstreamdemux
  -> per-stream queue / OSD / encode
  -> UDP RTP
  -> GstRtspServer
```

DeepStream owns the batched pre-demux work. C code owns post-demux branches,
the control API, RTSP wrapping, and lifecycle.

## Lifecycle

- Build with `./build.sh`.
- Run with `./run.sh`.
- Control API defaults to `CTRL_PORT=8080`.
- RTSP defaults to `RTSP_PORT=8554`.
- `GET /add_demo_stream` mounts the next `/sN` stream.
- `GET /status` reports capacity and active streams.

## Coding Rules

- Read the local DeepStream 8.0 samples before changing pipeline wiring.
- Keep this app C/GStreamer first; do not add Flask, Node, or Python media
  paths.
- Favor stock DeepStream/GStreamer elements for source, infer, demux, encode,
  payload, and sink.
- Keep options minimal and explicit: `main.c`, `app.c`, `branch.c`,
  `control.c`, `config.c`, plus shared headers.
- Preserve clear failure logs for pad requests, link failures, RTSP factory
  setup, and encoder initialization.
- Do not move batch-app behavior into `runtime`; they are
  separate runtimes with separate local canons.
