// ----------------------------------------------------------------------------
// C launchers. Each one packages the thread/block geometry; caller passes raw
// device pointers + dims + params.
// ----------------------------------------------------------------------------
__device__ __forceinline__ float2 sample_flow_bilinear(
    const int16_t* __restrict__ flow_vectors,
    int flowH, int flowW,
    int flow_pitch_bytes,
    int flow_grid,
    int frameH, int frameW,
    float y, float x)
{
  const float coverageW = fmaxf(1.0f, (float)(flowW * flow_grid));
  const float coverageH = fmaxf(1.0f, (float)(flowH * flow_grid));
  const float scaleX = fmaxf(0.0001f, (float)frameW / coverageW);
  const float scaleY = fmaxf(0.0001f, (float)frameH / coverageH);
  const float gx = fminf(fmaxf(((x + 0.5f) / scaleX) / (float)flow_grid - 0.5f, 0.0f), (float)(flowW - 1));
  const float gy = fminf(fmaxf(((y + 0.5f) / scaleY) / (float)flow_grid - 0.5f, 0.0f), (float)(flowH - 1));
  const int x0 = (int)floorf(gx);
  const int y0 = (int)floorf(gy);
  const int x1 = x0 + 1 < flowW ? x0 + 1 : x0;
  const int y1 = y0 + 1 < flowH ? y0 + 1 : y0;
  const float tx = gx - (float)x0;
  const float ty = gy - (float)y0;

  const int16_t* v00 = (const int16_t*)((const uint8_t*)flow_vectors + y0 * flow_pitch_bytes + x0 * (int)sizeof(int16_t) * 2);
  const int16_t* v10 = (const int16_t*)((const uint8_t*)flow_vectors + y0 * flow_pitch_bytes + x1 * (int)sizeof(int16_t) * 2);
  const int16_t* v01 = (const int16_t*)((const uint8_t*)flow_vectors + y1 * flow_pitch_bytes + x0 * (int)sizeof(int16_t) * 2);
  const int16_t* v11 = (const int16_t*)((const uint8_t*)flow_vectors + y1 * flow_pitch_bytes + x1 * (int)sizeof(int16_t) * 2);

  const float s = 1.0f / 32.0f;
  const float x00 = (float)v00[0] * s, y00 = (float)v00[1] * s;
  const float x10 = (float)v10[0] * s, y10 = (float)v10[1] * s;
  const float x01 = (float)v01[0] * s, y01 = (float)v01[1] * s;
  const float x11 = (float)v11[0] * s, y11 = (float)v11[1] * s;

  const float ax = mixf(x00, x10, tx);
  const float ay = mixf(y00, y10, tx);
  const float bx = mixf(x01, x11, tx);
  const float by = mixf(y01, y11, tx);
  return make_float2(mixf(ax, bx, ty) * scaleX, mixf(ay, by, ty) * scaleY);
}

__global__ void nvof_fruc_interpolate_kernel(
    const uint8_t* __restrict__ prev_rgba,
    const uint8_t* __restrict__ curr_rgba,
    const int16_t* __restrict__ forward_flow_vectors,
    const int16_t* __restrict__ reverse_flow_vectors,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float alpha,
    float confidence_scale
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  // NVOF vectors are S10.5 pixel offsets. Use forward flow for the previous
  // frame contribution and reverse flow for the current frame contribution.
  // This avoids the one-sided "rubber sheet" tear where both samples were
  // guessed from only the previous→current field.
  const float2 fwd = sample_flow_bilinear(
      forward_flow_vectors, flowH, flowW, forward_flow_pitch_bytes, flow_grid,
      H, W,
      (float)y, (float)x);
  const float2 rev = sample_flow_bilinear(
      reverse_flow_vectors, flowH, flowW, reverse_flow_pitch_bytes, flow_grid,
      H, W,
      (float)y, (float)x);

  const float px = (float)x - alpha * fwd.x;
  const float py = (float)y - alpha * fwd.y;
  const float cx = (float)x - (1.0f - alpha) * rev.x;
  const float cy = (float)y - (1.0f - alpha) * rev.y;
  const float4 prev_warp = sample_rgba_bilinear(prev_rgba, H, W, py, px);
  const float4 curr_warp = sample_rgba_bilinear(curr_rgba, H, W, cy, cx);
  const float4 prev_raw = load_rgba4(prev_rgba, H, W, y, x);
  const float4 curr_raw = load_rgba4(curr_rgba, H, W, y, x);

  // Be conservative about optical-flow warp, but never about time. A FRUC
  // fallback must still sit at alpha between the endpoints; falling back to
  // the nearest real frame creates a visible short/long motion alternation on
  // pans and tickers even when transport PTS is perfectly uniform.
  const float disagree = hypotf(fwd.x + rev.x, fwd.y + rev.y);
  const float motion = fmaxf(hypotf(fwd.x, fwd.y), hypotf(rev.x, rev.y));
  float confidence = 1.0f - smoothstep01(0.75f, 2.50f, disagree);
  confidence *= 1.0f - smoothstep01(40.0f, 140.0f, motion);
  const float endpoint_gate = smoothstep01(0.05f, 0.18f, fminf(alpha, 1.0f - alpha));
  confidence *= 0.86f * clamp01(confidence_scale) * endpoint_gate;

  const float4 warped = make_float4(
      mixf(prev_warp.x, curr_warp.x, alpha),
      mixf(prev_warp.y, curr_warp.y, alpha),
      mixf(prev_warp.z, curr_warp.z, alpha),
      1.0f);
  const float4 linear = make_float4(
      mixf(prev_raw.x, curr_raw.x, alpha),
      mixf(prev_raw.y, curr_raw.y, alpha),
      mixf(prev_raw.z, curr_raw.z, alpha),
      1.0f);

  write_rgba4(out_rgba, W, y, x,
              mixf(linear.x, warped.x, confidence),
              mixf(linear.y, warped.y, confidence),
              mixf(linear.z, warped.z, confidence));
}

__global__ void temporal_reconstruct_rgba_kernel(
    const uint8_t* __restrict__ prev_rgba,
    const uint8_t* __restrict__ curr_rgba,
    const int16_t* __restrict__ forward_flow_vectors,
    const int16_t* __restrict__ reverse_flow_vectors,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float temporal_strength,
    float edge_stability
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  float r[9], g[9], b[9], l[9];
  load_rgb(curr_rgba, H, W, y - 1, x - 1, &r[0], &g[0], &b[0]);
  load_rgb(curr_rgba, H, W, y - 1, x    , &r[1], &g[1], &b[1]);
  load_rgb(curr_rgba, H, W, y - 1, x + 1, &r[2], &g[2], &b[2]);
  load_rgb(curr_rgba, H, W, y    , x - 1, &r[3], &g[3], &b[3]);
  load_rgb(curr_rgba, H, W, y    , x    , &r[4], &g[4], &b[4]);
  load_rgb(curr_rgba, H, W, y    , x + 1, &r[5], &g[5], &b[5]);
  load_rgb(curr_rgba, H, W, y + 1, x - 1, &r[6], &g[6], &b[6]);
  load_rgb(curr_rgba, H, W, y + 1, x    , &r[7], &g[7], &b[7]);
  load_rgb(curr_rgba, H, W, y + 1, x + 1, &r[8], &g[8], &b[8]);
  float rmin = r[0], rmax = r[0], gmin = g[0], gmax = g[0], bmin = b[0], bmax = b[0];
  #pragma unroll
  for (int i = 0; i < 9; ++i) {
    l[i] = luma_of(r[i], g[i], b[i]);
    rmin = fminf(rmin, r[i]); rmax = fmaxf(rmax, r[i]);
    gmin = fminf(gmin, g[i]); gmax = fmaxf(gmax, g[i]);
    bmin = fminf(bmin, b[i]); bmax = fmaxf(bmax, b[i]);
  }

  const float gx = (l[2] + 2.0f*l[5] + l[8]) - (l[0] + 2.0f*l[3] + l[6]);
  const float gy = (l[6] + 2.0f*l[7] + l[8]) - (l[0] + 2.0f*l[1] + l[2]);
  const float edge = clamp01(sqrtf(gx * gx + gy * gy) * 3.0f);

  const float2 fwd = sample_flow_bilinear(
      forward_flow_vectors, flowH, flowW, forward_flow_pitch_bytes, flow_grid,
      H, W,
      (float)y, (float)x);
  const float2 rev = sample_flow_bilinear(
      reverse_flow_vectors, flowH, flowW, reverse_flow_pitch_bytes, flow_grid,
      H, W,
      (float)y, (float)x);

  const float4 hist_fwd = sample_rgba_bilinear(prev_rgba, H, W, (float)y - fwd.y, (float)x - fwd.x);
  const float4 hist_rev = sample_rgba_bilinear(prev_rgba, H, W, (float)y + rev.y, (float)x + rev.x);
  float hr = mixf(hist_fwd.x, hist_rev.x, 0.35f);
  float hg = mixf(hist_fwd.y, hist_rev.y, 0.35f);
  float hb = mixf(hist_fwd.z, hist_rev.z, 0.35f);

  // Clamp history into the current frame's local color neighborhood. This is
  // the practical TAA/DLSAA trick that keeps accumulation from ghosting across
  // disocclusions while still stabilizing subpixel line crawl.
  const float margin = mixf(0.010f, 0.030f, edge);
  hr = fminf(fmaxf(hr, rmin - margin), rmax + margin);
  hg = fminf(fmaxf(hg, gmin - margin), gmax + margin);
  hb = fminf(fmaxf(hb, bmin - margin), bmax + margin);

  const float disagree = hypotf(fwd.x + rev.x, fwd.y + rev.y);
  const float motion = fmaxf(hypotf(fwd.x, fwd.y), hypotf(rev.x, rev.y));
  const float dl = fabsf(luma_of(hr, hg, hb) - l[4]);
  const float dc = fabsf(hr - r[4]) + fabsf(hg - g[4]) + fabsf(hb - b[4]);
  float confidence = 1.0f - smoothstep01(0.80f, 2.80f, disagree);
  confidence *= 1.0f - smoothstep01(48.0f, 160.0f, motion);
  confidence *= 1.0f - smoothstep01(0.035f, 0.180f, dl + dc * 0.20f);

  // Edge stability intentionally favors strong line/corner pixels over flat
  // textures; flat areas get only mild shimmer cleanup.
  const float edge_gain = mixf(0.70f, 1.35f, edge * clamp01(edge_stability));
  const float blend = clamp01(temporal_strength) * 0.46f * confidence * edge_gain;

  write_rgba4(out_rgba, W, y, x,
              mixf(r[4], hr, blend),
              mixf(g[4], hg, blend),
              mixf(b[4], hb, blend));
}

__global__ void rgba_to_abgr_pitch_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_abgr,
    int H, int W,
    int out_pitch_bytes
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;
  const int src = (y * W + x) * 4;
  uint8_t* dst = out_abgr + y * out_pitch_bytes + x * 4;
  dst[0] = 255;
  dst[1] = in_rgba[src + 2];
  dst[2] = in_rgba[src + 1];
  dst[3] = in_rgba[src + 0];
}

void launch_nvof_fruc_interpolate(
    const void* prev_rgba, const void* curr_rgba,
    const void* forward_flow_vectors,
    const void* reverse_flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float alpha,
    float confidence_scale,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  nvof_fruc_interpolate_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)prev_rgba, (const uint8_t*)curr_rgba,
      (const int16_t*)forward_flow_vectors,
      (const int16_t*)reverse_flow_vectors,
      (uint8_t*)out_rgba,
      H, W, flowH, flowW,
      forward_flow_pitch_bytes, reverse_flow_pitch_bytes,
      flow_grid, alpha, confidence_scale
  );
}

void launch_temporal_reconstruct_rgba(
    const void* prev_rgba, const void* curr_rgba,
    const void* forward_flow_vectors,
    const void* reverse_flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int forward_flow_pitch_bytes,
    int reverse_flow_pitch_bytes,
    int flow_grid,
    float temporal_strength,
    float edge_stability,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  temporal_reconstruct_rgba_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)prev_rgba, (const uint8_t*)curr_rgba,
      (const int16_t*)forward_flow_vectors,
      (const int16_t*)reverse_flow_vectors,
      (uint8_t*)out_rgba,
      H, W, flowH, flowW,
      forward_flow_pitch_bytes, reverse_flow_pitch_bytes,
      flow_grid,
      temporal_strength, edge_stability
  );
}

void launch_rgba_to_abgr_pitch(
    const void* in_rgba,
    void* out_abgr,
    int H, int W,
    int out_pitch_bytes,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgba_to_abgr_pitch_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_abgr,
      H, W, out_pitch_bytes
  );
}

void launch_nv12_pre_vsr_filter(
    const void* y_plane, const void* uv_plane, void* out_rgba,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int input_format,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float cas_strength,
    void* stream
) {
  dim3 block(16, 16);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  nv12_pre_vsr_filter<<<grid, block, 0, s>>>(
      (const uint8_t*)y_plane, (const uint8_t*)uv_plane, (uint8_t*)out_rgba,
      srcH, srcW, y_pitch, uv_pitch, input_format, H, W,
      contrast, saturation, gamma_inv, cas_strength
  );
}

void launch_post_vsr_finalize_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float grain_strength,
    uint32_t frame_seed,
    float sharpen_strength,
    float contrast_boost,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  post_vsr_finalize_rgba<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_rgba,
      H, W, contrast, saturation, gamma_inv,
      grain_strength, frame_seed,
      sharpen_strength, contrast_boost
  );
}

void launch_rgba_to_nv12(
    const void* in_rgba, void* out_y, void* out_uv,
    int H, int W,
    int y_pitch, int uv_pitch,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgba_to_nv12_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_y, (uint8_t*)out_uv,
      H, W, y_pitch, uv_pitch
  );
}
