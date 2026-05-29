# d Repository Agent Canon

This repo contains generic GPU video processing runtimes. It is not a product,
provider, catalog, or UI repo.

## Scope

- `gst_shader_runtime_v1/` is the active reusable D runtime. It owns C/CUDA,
  TensorRT, Maxine, NVDEC/NVENC, NVOF, graph execution, source reception,
  timeline normalization, encoded sinks, status, and evidence.
- `batch_streaming_v1/` is a separate DeepStream batch-inference service. Treat
  its local `AGENTS.md` as the only instructions for that app.
- Product-specific control planes such as 99KS may launch D, but provider
  names, TV/catalog assumptions, account policy, and UI state must not become
  D architecture.

## Markdown Policy

`AGENTS.md` is the documentation format for this repo. Do not add README,
STANDARDS, MIGRATION, plan, or architecture markdown files. Put durable local
instructions into the nearest `AGENTS.md`; put generated/user API contracts into
the runtime's local agent file; put implementation detail next to code.

## Build Canon

Native D code builds inside the runtime container image. Do not compile D C,
CUDA, TensorRT, or Maxine code through host-side `make`, `gcc`, or `nvcc`.

The consuming control plane may wrap this with its own lifecycle, but that
wrapper must call the D Docker build/run path rather than bypassing it.

## Runtime Canon

- Graph JSON is the runtime contract.
- Source receivers normalize upstream timestamp defects before graph stages see
  frames.
- GPU residency is mandatory between decode and encode.
- CPU inference, host pixel copies, hidden product/provider branches, and codec
  fallback paths are not valid steady-state D behavior.
- Treat static files and VOD as first-class initial consumers. Live streams are
  another source mode, not the base assumption. Keep the graph language suitable
  for files, VOD, sports scoring, overlays, detectors, health monitors, live
  streams, and future TensorRT model chains.
