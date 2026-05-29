// ----------------------------------------------------------------------------
// Native bake post-kernel. VSR RGBA8 in (4K), CUDA NV12 encoder frame out.
//
// Pipeline per output pixel:
//   1. Sample 3×3 neighbourhood of VSR output.
//   2. Compute box-mean (gives us the low-frequency "blur").
//   3. Compute Sobel-on-luma gradient magnitude → edge factor in [0,1].
//   4. Local-contrast enhancement: detail = pixel - mean; pixel += boost ×
//      edge_factor × detail. Edge-gated so flat areas don't get cranked
//      (avoids the "HDR look gone wrong" plasticky enhancement).
//   5. Adaptive sharpen with halo guard: sharp = pixel + s × (pixel - mean),
//      then clamp to [min_nbr, max_nbr] of the 3×3 — overshoot can't
//      exceed local extrema so no ringing/halos.
//   6. Film grain (per-pixel hash PRNG).
//   7. RGB → NV12, full luma per pixel, chroma averaged per 2×2 block.
// ----------------------------------------------------------------------------
__device__ __forceinline__ void load_rgb(
    const uint8_t* __restrict__ in_rgba, int H, int W, int y, int x,
    float* r, float* g, float* b)
{
  if (y < 0) y = 0; else if (y >= H) y = H - 1;
  if (x < 0) x = 0; else if (x >= W) x = W - 1;
  const int idx = (y * W + x) * 4;
  *r = (float)in_rgba[idx + 0] * (1.0f / 255.0f);
  *g = (float)in_rgba[idx + 1] * (1.0f / 255.0f);
  *b = (float)in_rgba[idx + 2] * (1.0f / 255.0f);
}

__device__ __forceinline__ float4 load_rgba4(
    const uint8_t* __restrict__ in_rgba, int H, int W, int y, int x)
{
  if (y < 0) y = 0; else if (y >= H) y = H - 1;
  if (x < 0) x = 0; else if (x >= W) x = W - 1;
  const int idx = (y * W + x) * 4;
  return make_float4(
      (float)in_rgba[idx + 0] * (1.0f / 255.0f),
      (float)in_rgba[idx + 1] * (1.0f / 255.0f),
      (float)in_rgba[idx + 2] * (1.0f / 255.0f),
      1.0f);
}

__device__ __forceinline__ float4 sample_rgba_bilinear(
    const uint8_t* __restrict__ in_rgba, int H, int W, float fy, float fx)
{
  fx = fminf(fmaxf(fx, 0.0f), (float)(W - 1));
  fy = fminf(fmaxf(fy, 0.0f), (float)(H - 1));
  const int x0 = (int)floorf(fx);
  const int y0 = (int)floorf(fy);
  const int x1 = x0 + 1 < W ? x0 + 1 : x0;
  const int y1 = y0 + 1 < H ? y0 + 1 : y0;
  const float tx = fx - (float)x0;
  const float ty = fy - (float)y0;
  const float4 a = load_rgba4(in_rgba, H, W, y0, x0);
  const float4 b = load_rgba4(in_rgba, H, W, y0, x1);
  const float4 c = load_rgba4(in_rgba, H, W, y1, x0);
  const float4 d = load_rgba4(in_rgba, H, W, y1, x1);
  const float4 ab = make_float4(mixf(a.x, b.x, tx), mixf(a.y, b.y, tx), mixf(a.z, b.z, tx), 1.0f);
  const float4 cd = make_float4(mixf(c.x, d.x, tx), mixf(c.y, d.y, tx), mixf(c.z, d.z, tx), 1.0f);
  return make_float4(mixf(ab.x, cd.x, ty), mixf(ab.y, cd.y, ty), mixf(ab.z, cd.z, ty), 1.0f);
}

__device__ __forceinline__ void write_rgba4(
    uint8_t* __restrict__ out_rgba, int W, int y, int x, float r, float g, float b)
{
  const int idx = (y * W + x) * 4;
  out_rgba[idx + 0] = clamp01_to_u8(r);
  out_rgba[idx + 1] = clamp01_to_u8(g);
  out_rgba[idx + 2] = clamp01_to_u8(b);
  out_rgba[idx + 3] = 255;
}

__device__ __forceinline__ float luma_of(float r, float g, float b) {
  return luma_bt709(r, g, b);
}

__device__ __forceinline__ void apply_eq(
    float* r, float* g, float* b,
    float contrast, float saturation, float gamma_inv)
{
  if (gamma_inv != 1.0f) {
    *r = powf(fmaxf(*r, 1e-6f), gamma_inv);
    *g = powf(fmaxf(*g, 1e-6f), gamma_inv);
    *b = powf(fmaxf(*b, 1e-6f), gamma_inv);
  }
  if (contrast != 1.0f) {
    *r = (*r - 0.5f) * contrast + 0.5f;
    *g = (*g - 0.5f) * contrast + 0.5f;
    *b = (*b - 0.5f) * contrast + 0.5f;
  }
  if (saturation != 1.0f) {
    float luma = luma_of(*r, *g, *b);
    *r = luma + (*r - luma) * saturation;
    *g = luma + (*g - luma) * saturation;
    *b = luma + (*b - luma) * saturation;
  }
  *r = clamp01(*r);
  *g = clamp01(*g);
  *b = clamp01(*b);
}

__device__ __forceinline__ void apply_filmic_finish(
    float* r, float* g, float* b,
    int H, int W, int y, int x)
{
  float l = luma_of(*r, *g, *b);

  // Filmic S-curve with highlight shoulder and black-floor rolloff.
  *r = (*r * (1.05f + *r * 0.10f)) / (1.0f + *r * 0.22f);
  *g = (*g * (1.05f + *g * 0.10f)) / (1.0f + *g * 0.22f);
  *b = (*b * (1.05f + *b * 0.10f)) / (1.0f + *b * 0.22f);
  l = luma_of(*r, *g, *b);

  const float shadow = 1.0f - smoothstep01(0.12f, 0.34f, l);
  const float highlight = smoothstep01(0.62f, 0.94f, l);

  // Selective saturation: calmer shadows, slightly richer mids/highs.
  const float sat = mixf(0.88f, 1.04f, 1.0f - shadow);
  *r = l + (*r - l) * sat;
  *g = l + (*g - l) * sat;
  *b = l + (*b - l) * sat;

  // Warm highlights, cool shadows. Kept subtle so skin does not go orange.
  *r += 0.018f * highlight - 0.006f * shadow;
  *g += 0.006f * highlight;
  *b += 0.018f * shadow - 0.010f * highlight;

  // Gentle shadow toe. Avoid crushing: only pull the very bottom down.
  const float toe = 1.0f - 0.055f * shadow;
  *r *= toe; *g *= toe; *b *= toe;

  // Subtle vignette on the final 4K canvas.
  const float nx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f;
  const float ny = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
  const float d2 = nx * nx + ny * ny;
  const float vig = 1.0f - 0.055f * smoothstep01(0.48f, 1.35f, d2);
  *r = clamp01(*r * vig);
  *g = clamp01(*g * vig);
  *b = clamp01(*b * vig);
}

__device__ __forceinline__ void finalize_pixel(
    const uint8_t* __restrict__ in_rgba,
    int H, int W, int y, int x,
    float contrast, float saturation, float gamma_inv,
    float sharpen_strength, float contrast_boost,
    float grain_strength, uint32_t frame_seed,
    float* out_r, float* out_g, float* out_b)
{
  (void)contrast;
  (void)saturation;
  (void)gamma_inv;

  // 3×3 neighbourhood
  float r[9], g[9], b[9];
  load_rgb(in_rgba, H, W, y - 1, x - 1, &r[0], &g[0], &b[0]);
  load_rgb(in_rgba, H, W, y - 1, x    , &r[1], &g[1], &b[1]);
  load_rgb(in_rgba, H, W, y - 1, x + 1, &r[2], &g[2], &b[2]);
  load_rgb(in_rgba, H, W, y    , x - 1, &r[3], &g[3], &b[3]);
  load_rgb(in_rgba, H, W, y    , x    , &r[4], &g[4], &b[4]);
  load_rgb(in_rgba, H, W, y    , x + 1, &r[5], &g[5], &b[5]);
  load_rgb(in_rgba, H, W, y + 1, x - 1, &r[6], &g[6], &b[6]);
  load_rgb(in_rgba, H, W, y + 1, x    , &r[7], &g[7], &b[7]);
  load_rgb(in_rgba, H, W, y + 1, x + 1, &r[8], &g[8], &b[8]);

  // Box mean (low-pass)
  float mr = 0, mg = 0, mb = 0;
  #pragma unroll
  for (int i = 0; i < 9; ++i) { mr += r[i]; mg += g[i]; mb += b[i]; }
  mr *= (1.0f / 9.0f); mg *= (1.0f / 9.0f); mb *= (1.0f / 9.0f);

  // Sobel on luma → edge magnitude in [0, ~1]. Used to gate both the
  // local-contrast boost and the strength of sharpening, so flat areas
  // stay flat and only real edges get enhanced.
  float L[9];
  #pragma unroll
  for (int i = 0; i < 9; ++i) L[i] = luma_of(r[i], g[i], b[i]);
  float gx = (L[2] + 2.0f*L[5] + L[8]) - (L[0] + 2.0f*L[3] + L[6]);
  float gy = (L[6] + 2.0f*L[7] + L[8]) - (L[0] + 2.0f*L[1] + L[2]);
  float edge = sqrtf(gx * gx + gy * gy) * 0.25f;  // /4 to keep in [0,~1]
  edge = clamp01(edge * 4.0f);  // map ~[0,0.25] → [0,1]

  const float stair = clamp01(fabsf(L[1] + L[7] - 2.0f * L[4]) * 6.0f + fabsf(L[3] + L[5] - 2.0f * L[4]) * 6.0f);
  const float compression_guard = smoothstep01(0.05f, 0.22f, stair) * (1.0f - smoothstep01(0.34f, 0.62f, edge));

  // Local contrast: detail boost gated by edge.
  // contrast_boost typical 0.35 (the "Bravia pop"); on flat areas edge≈0
  // so no boost. On detail-heavy areas edge≈1 and we get +35% detail.
  const float contrast_safe = contrast_boost * (1.0f - 0.20f * compression_guard);
  float cr = r[4] + contrast_safe * edge * (r[4] - mr);
  float cg = g[4] + contrast_safe * edge * (g[4] - mg);
  float cb = b[4] + contrast_safe * edge * (b[4] - mb);

  // Adaptive sharpen on the contrast-boosted pixel. sharpen_strength is
  // also edge-gated (sharper on edges, untouched on flat). Halo guard:
  // clamp to local 3×3 min/max so the sharpened value can never overshoot
  // the neighbourhood — kills ringing.
  float sg = sharpen_strength * (0.55f + 0.85f * edge) * (1.0f - 0.25f * compression_guard);
  float sr = cr + sg * (cr - mr);
  float sg_ = cg + sg * (cg - mg);
  float sb = cb + sg * (cb - mb);

  float rmin = r[0], rmax = r[0], gmin = g[0], gmax = g[0], bmin = b[0], bmax = b[0];
  #pragma unroll
  for (int i = 1; i < 9; ++i) {
    rmin = fminf(rmin, r[i]); rmax = fmaxf(rmax, r[i]);
    gmin = fminf(gmin, g[i]); gmax = fmaxf(gmax, g[i]);
    bmin = fminf(bmin, b[i]); bmax = fmaxf(bmax, b[i]);
  }
  sr  = fminf(fmaxf(sr,  rmin), rmax);
  sg_ = fminf(fmaxf(sg_, gmin), gmax);
  sb  = fminf(fmaxf(sb,  bmin), bmax);

  if (grain_strength > 0.0f) {
    uint32_t seed = frame_seed ^ ((uint32_t)x * 0x9E3779B9u) ^ ((uint32_t)y * 0x85EBCA77u);
    const float l = luma_of(sr, sg_, sb);
    const float grain_gate = 0.35f + 0.65f * smoothstep01(0.10f, 0.60f, l);
    float n = prng_gauss(seed) * grain_strength * grain_gate;
    sr += n; sg_ += n; sb += n;
  }

  *out_r = clamp01(sr);
  *out_g = clamp01(sg_);
  *out_b = clamp01(sb);
}

__global__ void post_vsr_finalize_rgba(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float grain_strength,
    uint32_t frame_seed,
    float sharpen_strength,
    float contrast_boost
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  float r, g, b;
  finalize_pixel(in_rgba, H, W, y, x, contrast, saturation, gamma_inv,
                 sharpen_strength, contrast_boost,
                 grain_strength, frame_seed, &r, &g, &b);
  write_rgba4(out_rgba, W, y, x, r, g, b);
}

__global__ void rgba_to_nv12_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_y,
    uint8_t* __restrict__ out_uv,
    int H, int W,
    int y_pitch, int uv_pitch
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  const float4 p0 = load_rgba4(in_rgba, H, W, y, x);
  uint8_t y8, u8, v8;
  rgb_to_nv12_values(p0.x, p0.y, p0.z, &y8, &u8, &v8);
  out_y[y * y_pitch + x] = y8;

  if (((x | y) & 1) == 0) {
    const float4 p1 = load_rgba4(in_rgba, H, W, y    , x + 1);
    const float4 p2 = load_rgba4(in_rgba, H, W, y + 1, x    );
    const float4 p3 = load_rgba4(in_rgba, H, W, y + 1, x + 1);
    const float ar = (p0.x + p1.x + p2.x + p3.x) * 0.25f;
    const float ag = (p0.y + p1.y + p2.y + p3.y) * 0.25f;
    const float ab = (p0.z + p1.z + p2.z + p3.z) * 0.25f;
    rgb_to_nv12_values(ar, ag, ab, &y8, &u8, &v8);
    uint8_t* uv = out_uv + (y >> 1) * uv_pitch + x;
    uv[0] = u8;
    uv[1] = v8;
  }
}

