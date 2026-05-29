__device__ __forceinline__ float prng_gauss(uint32_t seed) {
  // Two LCG steps + Box-Muller-ish polar approximation (we don't need true
  // gaussian quality for film grain; a triangle dither subbed for it works
  // visually identical and avoids the log/sqrt).
  uint32_t a = seed * 1664525u + 1013904223u;
  uint32_t b = a * 1664525u + 1013904223u;
  // Two uniforms in [-1, 1)
  float u1 = (float)(a & 0xFFFFu) / 32768.0f - 1.0f;
  float u2 = (float)(b & 0xFFFFu) / 32768.0f - 1.0f;
  // Triangle distribution ≈ gaussian for our purposes (std ≈ 0.41 of magnitude)
  return (u1 + u2) * 0.5f;
}

__device__ __forceinline__ float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

__device__ __forceinline__ uint8_t clamp_u8(float v) {
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  return (uint8_t)(v + 0.5f);
}

__device__ __forceinline__ uint8_t clamp01_to_u8(float v) {
  return clamp_u8(clamp01(v) * 255.0f);
}

__device__ __forceinline__ float luma_bt709(float r, float g, float b) {
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

__device__ __forceinline__ float mixf(float a, float b, float t) {
  return a + (b - a) * t;
}

__device__ __forceinline__ float smoothstep01(float edge0, float edge1, float x) {
  float t = clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0f - 2.0f * t);
}

enum {
  DPROC_YUV420_NV12 = 0,
  DPROC_YUV420_P010 = 1,
  DPROC_YUV420_P016 = 2,
};

__device__ __forceinline__ void yuv420_rgb_at(
    const uint8_t* __restrict__ y_plane,
    const uint8_t* __restrict__ uv_plane,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int outH, int outW,
    int input_format,
    int oy, int ox,
    float* r, float* g, float* b
) {
  if (oy < 0) oy = 0; if (oy >= outH) oy = outH - 1;
  if (ox < 0) ox = 0; if (ox >= outW) ox = outW - 1;

  int sx = (int)floorf(((float)ox + 0.5f) * (float)srcW / (float)outW);
  int sy = (int)floorf(((float)oy + 0.5f) * (float)srcH / (float)outH);
  if (sx < 0) sx = 0; if (sx >= srcW) sx = srcW - 1;
  if (sy < 0) sy = 0; if (sy >= srcH) sy = srcH - 1;

  const int uv_x = (sx >> 1) << 1;
  const int uv_y = sy >> 1;

  float yf, uf, vf;
  if (input_format == DPROC_YUV420_P010) {
    const uint16_t* y16 = (const uint16_t*)y_plane;
    const uint16_t* uv16 = (const uint16_t*)uv_plane;
    const int y_pitch_px = y_pitch >> 1;
    const int uv_pitch_px = uv_pitch >> 1;
    // P010 stores 10-bit limited-range samples in the high bits.
    const int Y = (int)(y16[sy * y_pitch_px + sx] >> 6);
    const int U = (int)(uv16[uv_y * uv_pitch_px + uv_x + 0] >> 6);
    const int V = (int)(uv16[uv_y * uv_pitch_px + uv_x + 1] >> 6);
    yf = fmaxf(0.0f, ((float)Y - 64.0f) * (1.0f / 876.0f));
    uf = ((float)U - 512.0f) * (1.0f / 896.0f);
    vf = ((float)V - 512.0f) * (1.0f / 896.0f);
  } else if (input_format == DPROC_YUV420_P016) {
    const uint16_t* y16 = (const uint16_t*)y_plane;
    const uint16_t* uv16 = (const uint16_t*)uv_plane;
    const int y_pitch_px = y_pitch >> 1;
    const int uv_pitch_px = uv_pitch >> 1;
    const int Y = (int)y16[sy * y_pitch_px + sx];
    const int U = (int)uv16[uv_y * uv_pitch_px + uv_x + 0];
    const int V = (int)uv16[uv_y * uv_pitch_px + uv_x + 1];
    yf = fmaxf(0.0f, ((float)Y - 4096.0f) * (1.0f / 56064.0f));
    uf = ((float)U - 32768.0f) * (1.0f / 57344.0f);
    vf = ((float)V - 32768.0f) * (1.0f / 57344.0f);
  } else {
    const uint8_t Y = y_plane[sy * y_pitch + sx];
    const uint8_t U = uv_plane[uv_y * uv_pitch + uv_x + 0];
    const uint8_t V = uv_plane[uv_y * uv_pitch + uv_x + 1];
    yf = fmaxf(0.0f, ((float)Y - 16.0f) * (1.0f / 219.0f));
    uf = ((float)U - 128.0f) * (1.0f / 224.0f);
    vf = ((float)V - 128.0f) * (1.0f / 224.0f);
  }

  // BT.709 limited-range YUV420 -> RGB float [0,1].
  *r = clamp01(yf + 1.5748f * vf);
  *g = clamp01(yf - 0.1873f * uf - 0.4681f * vf);
  *b = clamp01(yf + 1.8556f * uf);
}

__device__ __forceinline__ void rgb_to_nv12_values(float r, float g, float b, uint8_t* y, uint8_t* u, uint8_t* v) {
  r = clamp01(r); g = clamp01(g); b = clamp01(b);
  const float yf = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  const float uf = (b - yf) * (1.0f / 1.8556f);
  const float vf = (r - yf) * (1.0f / 1.5748f);
  *y = clamp_u8(16.0f + 219.0f * yf);
  *u = clamp_u8(128.0f + 224.0f * uf);
  *v = clamp_u8(128.0f + 224.0f * vf);
}

__device__ __forceinline__ void sr_rgba_with_grain(
    const uint8_t* __restrict__ in_rgba,
    int H, int W,
    int y, int x,
    float grain_strength,
    uint32_t frame_seed,
    float* r, float* g, float* b
) {
  if (y < 0) y = 0; if (y >= H) y = H - 1;
  if (x < 0) x = 0; if (x >= W) x = W - 1;
  const int idx = (y * W + x) * 4;
  *r = (float)in_rgba[idx + 0] * (1.0f / 255.0f);
  *g = (float)in_rgba[idx + 1] * (1.0f / 255.0f);
  *b = (float)in_rgba[idx + 2] * (1.0f / 255.0f);
  if (grain_strength > 0.0f) {
    uint32_t seed = frame_seed ^ ((uint32_t)x * 0x9E3779B9u) ^ ((uint32_t)y * 0x85EBCA77u);
    const float n = prng_gauss(seed) * grain_strength;
    *r += n; *g += n; *b += n;
  }
}

