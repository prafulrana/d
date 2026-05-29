// ----------------------------------------------------------------------------
// Custom shader stage.
//
// One configurable 4K RGBA pass: edge-aware local tone, subtle highlight/shadow
// color separation, and a gentle center-weighted exposure lift. The graph
// selects whether it runs and passes intensity; the frame stays in CUDA memory.
// ----------------------------------------------------------------------------
__global__ void custom_shader_rgba_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    float intensity,
    uint32_t frame_seed
) {
  (void)frame_seed;
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  float r[9], g[9], b[9], l[9];
  load_rgb(in_rgba, H, W, y - 1, x - 1, &r[0], &g[0], &b[0]);
  load_rgb(in_rgba, H, W, y - 1, x    , &r[1], &g[1], &b[1]);
  load_rgb(in_rgba, H, W, y - 1, x + 1, &r[2], &g[2], &b[2]);
  load_rgb(in_rgba, H, W, y    , x - 1, &r[3], &g[3], &b[3]);
  load_rgb(in_rgba, H, W, y    , x    , &r[4], &g[4], &b[4]);
  load_rgb(in_rgba, H, W, y    , x + 1, &r[5], &g[5], &b[5]);
  load_rgb(in_rgba, H, W, y + 1, x - 1, &r[6], &g[6], &b[6]);
  load_rgb(in_rgba, H, W, y + 1, x    , &r[7], &g[7], &b[7]);
  load_rgb(in_rgba, H, W, y + 1, x + 1, &r[8], &g[8], &b[8]);

  float mean = 0.0f;
  #pragma unroll
  for (int i = 0; i < 9; i++) {
    l[i] = luma_of(r[i], g[i], b[i]);
    mean += l[i];
  }
  mean *= 1.0f / 9.0f;
  const float gx = (l[2] + 2.0f*l[5] + l[8]) - (l[0] + 2.0f*l[3] + l[6]);
  const float gy = (l[6] + 2.0f*l[7] + l[8]) - (l[0] + 2.0f*l[1] + l[2]);
  const float edge = clamp01(sqrtf(gx * gx + gy * gy) * 2.4f);
  const float t = clamp01(intensity);

  float rr = r[4];
  float gg = g[4];
  float bb = b[4];
  const float lum = l[4];
  const float detail = (lum - mean) * edge * 0.30f * t;
  rr += detail;
  gg += detail;
  bb += detail;

  const float shadow = 1.0f - smoothstep01(0.16f, 0.42f, lum);
  const float highlight = smoothstep01(0.58f, 0.92f, lum);
  rr += (0.020f * highlight - 0.006f * shadow) * t;
  gg += (0.006f * highlight) * t;
  bb += (0.014f * shadow - 0.010f * highlight) * t;

  const float nx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f;
  const float ny = ((float)y + 0.5f) / (float)H * 2.0f - 1.0f;
  const float center = 1.0f - smoothstep01(0.20f, 1.25f, nx * nx + ny * ny);
  const float lift = (0.012f * center - 0.010f * (1.0f - center)) * t;
  rr += lift;
  gg += lift;
  bb += lift;

  write_rgba4(out_rgba, W, y, x, clamp01(rr), clamp01(gg), clamp01(bb));
}

void launch_custom_shader_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W, float intensity, uint32_t frame_seed, void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  custom_shader_rgba_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_rgba,
      H, W, intensity, frame_seed
  );
}

// (FXAA removed — DLSAA temporal_reconstruct_rgba already provides edge AA
// via NVOF-warped prev-frame blending. Adding a spatial FXAA on top would
// just double-smooth and soften real detail.)
#if 0
__global__ void fxaa_lite_rgba_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    float strength
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;
  const int idx = ((size_t)y * W + x) * 4;
  int xm = x > 0 ? x - 1 : x;
  int xp = x + 1 < W ? x + 1 : x;
  int ym = y > 0 ? y - 1 : y;
  int yp = y + 1 < H ? y + 1 : y;
  #define LUMA_AT(rx, ry) ({ \
    const uint8_t* p = in_rgba + ((size_t)(ry) * W + (rx)) * 4; \
    (0.2126f * (float)p[0] + 0.7152f * (float)p[1] + 0.0722f * (float)p[2]); \
  })
  float Lc  = LUMA_AT(x , y );
  float Ll  = LUMA_AT(xm, y );
  float Lr  = LUMA_AT(xp, y );
  float Lu  = LUMA_AT(x , ym);
  float Ld  = LUMA_AT(x , yp);
  float Lul = LUMA_AT(xm, ym);
  float Lur = LUMA_AT(xp, ym);
  float Ldl = LUMA_AT(xm, yp);
  float Ldr = LUMA_AT(xp, yp);
  #undef LUMA_AT

  // Sobel gradient magnitude — edge strength.
  float gx = (Lur + 2.0f*Lr + Ldr) - (Lul + 2.0f*Ll + Ldl);
  float gy = (Ldl + 2.0f*Ld + Ldr) - (Lul + 2.0f*Lu + Lur);
  float gmag = sqrtf(gx*gx + gy*gy);
  // Threshold: edges only. Tuned for 0..255 luma input.
  if (gmag < 8.0f) {
    // Flat — pass through.
    out_rgba[idx + 0] = in_rgba[idx + 0];
    out_rgba[idx + 1] = in_rgba[idx + 1];
    out_rgba[idx + 2] = in_rgba[idx + 2];
    out_rgba[idx + 3] = 255;
    return;
  }
  // Edge tangent direction (perpendicular to gradient). Sample along tangent.
  float inv = 1.0f / fmaxf(gmag, 1e-3f);
  float tx = -gy * inv;  // tangent x = -grad y
  float ty =  gx * inv;  // tangent y =  grad x

  // Sample +/- 1 pixel along tangent (bilinear). Average with center.
  auto sample_rgb = [&] __device__ (float fx_, float fy_, float* outR, float* outG, float* outB) {
    int x0 = (int)floorf(fx_);
    int y0 = (int)floorf(fy_);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float ax = fx_ - (float)x0;
    float ay = fy_ - (float)y0;
    if (x0 < 0) x0 = 0; if (x0 >= W) x0 = W - 1;
    if (x1 < 0) x1 = 0; if (x1 >= W) x1 = W - 1;
    if (y0 < 0) y0 = 0; if (y0 >= H) y0 = H - 1;
    if (y1 < 0) y1 = 0; if (y1 >= H) y1 = H - 1;
    const uint8_t* p00 = in_rgba + ((size_t)y0 * W + x0) * 4;
    const uint8_t* p10 = in_rgba + ((size_t)y0 * W + x1) * 4;
    const uint8_t* p01 = in_rgba + ((size_t)y1 * W + x0) * 4;
    const uint8_t* p11 = in_rgba + ((size_t)y1 * W + x1) * 4;
    float r0 = (1-ax)*p00[0] + ax*p10[0];
    float r1 = (1-ax)*p01[0] + ax*p11[0];
    *outR = (1-ay)*r0 + ay*r1;
    float g0 = (1-ax)*p00[1] + ax*p10[1];
    float g1 = (1-ax)*p01[1] + ax*p11[1];
    *outG = (1-ay)*g0 + ay*g1;
    float b0 = (1-ax)*p00[2] + ax*p10[2];
    float b1 = (1-ax)*p01[2] + ax*p11[2];
    *outB = (1-ay)*b0 + ay*b1;
  };

  float rp, gp, bp, rn, gn, bn;
  sample_rgb((float)x + tx, (float)y + ty, &rp, &gp, &bp);
  sample_rgb((float)x - tx, (float)y - ty, &rn, &gn, &bn);
  // Blend center with tangent neighbors. Weight by edge strength capped to 1.
  float w = fminf(strength * (gmag * (1.0f/40.0f)), strength);
  float rc = (float)in_rgba[idx + 0];
  float gc = (float)in_rgba[idx + 1];
  float bc = (float)in_rgba[idx + 2];
  float r = rc * (1.0f - w) + (rp + rn) * 0.5f * w;
  float g = gc * (1.0f - w) + (gp + gn) * 0.5f * w;
  float b = bc * (1.0f - w) + (bp + bn) * 0.5f * w;
  if (r < 0.0f) r = 0.0f; if (r > 255.0f) r = 255.0f;
  if (g < 0.0f) g = 0.0f; if (g > 255.0f) g = 255.0f;
  if (b < 0.0f) b = 0.0f; if (b > 255.0f) b = 255.0f;
  out_rgba[idx + 0] = (uint8_t)(r + 0.5f);
  out_rgba[idx + 1] = (uint8_t)(g + 0.5f);
  out_rgba[idx + 2] = (uint8_t)(b + 0.5f);
  out_rgba[idx + 3] = 255;
}

#endif // FXAA removed

// ----------------------------------------------------------------------------
// NVOF-guided temporal denoise (kills shadow / dark-region noise).
//
// Reads prev frame warped by flow (via the existing sample_flow_bilinear at
// 4K), and the current frame. Blends them where the flow is reliable AND the
// region is dark (noise is most visible in shadows). Edges + bright regions
// pass through unchanged (preserves detail).
//
// Differs from temporal_reconstruct_rgba which is edge-stability oriented;
// this one is noise-reduction oriented and is keyed to LUMA — denoises only
// where the eye actually sees noise.
// ----------------------------------------------------------------------------
