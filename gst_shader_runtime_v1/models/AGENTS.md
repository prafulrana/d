# Model Mount Agent Canon

Model binaries are runtime artifacts, not source. Mount or bake them into this
directory when running the container:

- TensorRT CUGAN/ESRGAN engines: `models/upscalers/*.engine`
- Maxine Video Effects runtime/model files: `models/maxine-video/`
- Maxine Audio Effects runtime/model files: `models/maxine-audio/`

The graph references model bins by stable stage params. The runtime must fail
when a selected model is missing; it must not silently fall back to CPU or H.264.
