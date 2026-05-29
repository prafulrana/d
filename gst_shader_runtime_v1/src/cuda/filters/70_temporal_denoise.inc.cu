__global__ void temporal_denoise_rgba_kernel(
    const uint8_t* __restrict__ curr_rgba,
    const uint8_t* __restrict__ prev_rgba,
    const int16_t* __restrict__ flow_vectors,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    int flowH, int flowW,
    int flow_pitch_bytes,
    int flow_grid,
    float strength
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;
  const int idx = ((size_t)y * W + x) * 4;
  float cr = (float)curr_rgba[idx + 0];
  float cg = (float)curr_rgba[idx + 1];
  float cb = (float)curr_rgba[idx + 2];
  float clum = 0.2126f * cr + 0.7152f * cg + 0.0722f * cb;

  // Sample flow at this pixel.
  float2 flow = sample_flow_bilinear(flow_vectors, flowH, flowW,
                                     flow_pitch_bytes, flow_grid,
                                     H, W,
                                     (float)y, (float)x);
  // Warp prev: prev_at(x - fx, y - fy) — sample where this pixel CAME from.
  float px = (float)x - flow.x;
  float py = (float)y - flow.y;
  if (px < 0.0f) px = 0.0f; if (px > (float)(W - 1)) px = (float)(W - 1);
  if (py < 0.0f) py = 0.0f; if (py > (float)(H - 1)) py = (float)(H - 1);
  int x0 = (int)floorf(px); int x1 = x0 + 1 < W ? x0 + 1 : x0;
  int y0 = (int)floorf(py); int y1 = y0 + 1 < H ? y0 + 1 : y0;
  float ax = px - (float)x0;
  float ay = py - (float)y0;
  const uint8_t* p00 = prev_rgba + ((size_t)y0 * W + x0) * 4;
  const uint8_t* p10 = prev_rgba + ((size_t)y0 * W + x1) * 4;
  const uint8_t* p01 = prev_rgba + ((size_t)y1 * W + x0) * 4;
  const uint8_t* p11 = prev_rgba + ((size_t)y1 * W + x1) * 4;
  float r0 = (1-ax)*p00[0] + ax*p10[0];
  float r1 = (1-ax)*p01[0] + ax*p11[0];
  float pr = (1-ay)*r0 + ay*r1;
  float g0 = (1-ax)*p00[1] + ax*p10[1];
  float g1 = (1-ax)*p01[1] + ax*p11[1];
  float pg = (1-ay)*g0 + ay*g1;
  float b0 = (1-ax)*p00[2] + ax*p10[2];
  float b1 = (1-ax)*p01[2] + ax*p11[2];
  float pb = (1-ay)*b0 + ay*b1;

  // Confidence: low when warped prev differs a LOT from current (occlusion /
  // bad flow). High when warped prev matches current closely (stable region).
  float plum = 0.2126f * pr + 0.7152f * pg + 0.0722f * pb;
  float diff = fabsf(clum - plum);
  float conf = expf(-(diff * diff) * (1.0f / 200.0f)); // sigma ~14 luma units

  // Luma weight: noise most visible in dark regions (lum < 80/255).
  float lwt = 1.0f - smoothstep01(60.0f, 130.0f, clum);

  // Effective blend weight: bounded by strength × conf × luma_wt.
  float w = strength * conf * lwt;
  if (w > 0.7f) w = 0.7f; // never fully replace current — preserve motion fidelity.

  float r = cr * (1.0f - w) + pr * w;
  float g = cg * (1.0f - w) + pg * w;
  float b = cb * (1.0f - w) + pb * w;
  if (r < 0.0f) r = 0.0f; if (r > 255.0f) r = 255.0f;
  if (g < 0.0f) g = 0.0f; if (g > 255.0f) g = 255.0f;
  if (b < 0.0f) b = 0.0f; if (b > 255.0f) b = 255.0f;
  out_rgba[idx + 0] = (uint8_t)(r + 0.5f);
  out_rgba[idx + 1] = (uint8_t)(g + 0.5f);
  out_rgba[idx + 2] = (uint8_t)(b + 0.5f);
  out_rgba[idx + 3] = 255;
}

void launch_temporal_denoise_rgba(
    const void* curr_rgba, const void* prev_rgba,
    const void* flow_vectors,
    void* out_rgba,
    int H, int W,
    int flowH, int flowW,
    int flow_pitch_bytes,
    int flow_grid,
    float strength,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  temporal_denoise_rgba_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)curr_rgba, (const uint8_t*)prev_rgba,
      (const int16_t*)flow_vectors,
      (uint8_t*)out_rgba,
      H, W, flowH, flowW, flow_pitch_bytes, flow_grid, strength
  );
}

