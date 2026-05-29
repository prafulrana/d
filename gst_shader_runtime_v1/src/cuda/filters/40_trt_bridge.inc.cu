// ----------------------------------------------------------------------------
// ESRGAN bridge kernels.
//
// Pre-ESRGAN: NV12 / P010 / P016 source (any HxW) → RGB float32 NCHW at the
// engine's fixed input dims. Output layout is one contiguous block:
// plane 0 (R): out[0 .. W*H), plane 1 (G): out[W*H .. 2*W*H), plane 2 (B): out[2*W*H..).
// Values are in [0, 1] (Real-ESRGAN convention).
//
// Post-ESRGAN: RGB float32 NCHW at the engine's fixed output dims → RGBA8
// interleaved at canonical 4K (3840×2160). Bilinear resample handles the
// engine-out → 4K mapping (engine-out can be smaller, equal, or larger).
// ----------------------------------------------------------------------------

__global__ void nv12_to_rgb_chw_clamped_kernel(
    const uint8_t* __restrict__ y_plane,
    const uint8_t* __restrict__ uv_plane,
    float* __restrict__ out_rgb_chw,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int input_format,
    int dstH, int dstW
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dstW || y >= dstH) return;

  float r, g, b;
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch,
                dstH, dstW, input_format, y, x, &r, &g, &b);

  const int idx = y * dstW + x;
  const int plane = dstH * dstW;
  out_rgb_chw[0 * plane + idx] = fminf(fmaxf(r, 0.0f), 1.0f);
  out_rgb_chw[1 * plane + idx] = fminf(fmaxf(g, 0.0f), 1.0f);
  out_rgb_chw[2 * plane + idx] = fminf(fmaxf(b, 0.0f), 1.0f);
}

__device__ __forceinline__ float sample_plane_bilinear(
    const float* plane, int H, int W, float fy, float fx)
{
  if (fx < 0.0f) fx = 0.0f;
  if (fy < 0.0f) fy = 0.0f;
  if (fx > (float)(W - 1)) fx = (float)(W - 1);
  if (fy > (float)(H - 1)) fy = (float)(H - 1);
  int x0 = (int)floorf(fx);
  int y0 = (int)floorf(fy);
  int x1 = x0 + 1 < W ? x0 + 1 : x0;
  int y1 = y0 + 1 < H ? y0 + 1 : y0;
  float tx = fx - (float)x0;
  float ty = fy - (float)y0;
  float v00 = plane[y0 * W + x0];
  float v10 = plane[y0 * W + x1];
  float v01 = plane[y1 * W + x0];
  float v11 = plane[y1 * W + x1];
  float a = v00 + (v10 - v00) * tx;
  float c = v01 + (v11 - v01) * tx;
  return a + (c - a) * ty;
}

__device__ __forceinline__ float sample_plane_normalized(
    const float* plane, int srcH, int srcW, int dstH, int dstW, float fy, float fx)
{
  const float center = sample_plane_bilinear(plane, srcH, srcW, fy, fx);
  const float scale_x = (float)srcW / (float)dstW;
  const float scale_y = (float)srcH / (float)dstH;
  if (scale_x <= 1.05f && scale_y <= 1.05f) return center;

  const float ox = fmaxf(0.25f, 0.25f * scale_x);
  const float oy = fmaxf(0.25f, 0.25f * scale_y);
  const float box =
      sample_plane_bilinear(plane, srcH, srcW, fy - oy, fx - ox) +
      sample_plane_bilinear(plane, srcH, srcW, fy - oy, fx + ox) +
      sample_plane_bilinear(plane, srcH, srcW, fy + oy, fx - ox) +
      sample_plane_bilinear(plane, srcH, srcW, fy + oy, fx + ox);
  return center * 0.25f + box * 0.1875f;
}

__global__ void rgb_chw_to_rgba8_resampled_kernel(
    const float* __restrict__ in_rgb_chw,
    uint8_t* __restrict__ out_rgba,
    int srcH, int srcW,
    int dstH, int dstW
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dstW || y >= dstH) return;

  const float fx = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f;
  const float fy = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;

  const int plane = srcH * srcW;
  float r = sample_plane_normalized(in_rgb_chw + 0 * plane, srcH, srcW, dstH, dstW, fy, fx);
  float g = sample_plane_normalized(in_rgb_chw + 1 * plane, srcH, srcW, dstH, dstW, fy, fx);
  float b = sample_plane_normalized(in_rgb_chw + 2 * plane, srcH, srcW, dstH, dstW, fy, fx);
  r = fminf(fmaxf(r, 0.0f), 1.0f);
  g = fminf(fmaxf(g, 0.0f), 1.0f);
  b = fminf(fmaxf(b, 0.0f), 1.0f);

  uint8_t* p = out_rgba + ((size_t)y * dstW + x) * 4;
  p[0] = (uint8_t)(r * 255.0f + 0.5f);
  p[1] = (uint8_t)(g * 255.0f + 0.5f);
  p[2] = (uint8_t)(b * 255.0f + 0.5f);
  p[3] = 255;
}

__global__ void rgba8_to_rgb_chw_resampled_kernel(
    const uint8_t* __restrict__ in_rgba,
    float* __restrict__ out_rgb_chw,
    int srcH, int srcW,
    int dstH, int dstW
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dstW || y >= dstH) return;

  const float fx = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f;
  const float fy = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;
  const float4 rgba = sample_rgba_bilinear(in_rgba, srcH, srcW, fy, fx);
  const int plane = dstH * dstW;
  const int idx = y * dstW + x;
  out_rgb_chw[0 * plane + idx] = rgba.x;
  out_rgb_chw[1 * plane + idx] = rgba.y;
  out_rgb_chw[2 * plane + idx] = rgba.z;
}

__global__ void rgba8_to_rgba8_resampled_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_rgba,
    int srcH, int srcW,
    int dstH, int dstW
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dstW || y >= dstH) return;

  const float fx = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f;
  const float fy = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;
  const float4 rgba = sample_rgba_bilinear(in_rgba, srcH, srcW, fy, fx);
  uint8_t* p = out_rgba + ((size_t)y * dstW + x) * 4;
  p[0] = (uint8_t)(fminf(fmaxf(rgba.x, 0.0f), 1.0f) * 255.0f + 0.5f);
  p[1] = (uint8_t)(fminf(fmaxf(rgba.y, 0.0f), 1.0f) * 255.0f + 0.5f);
  p[2] = (uint8_t)(fminf(fmaxf(rgba.z, 0.0f), 1.0f) * 255.0f + 0.5f);
  p[3] = 255;
}

__global__ void rgb_chw_to_rgb_chw_resampled_kernel(
    const float* __restrict__ in_rgb_chw,
    float* __restrict__ out_rgb_chw,
    int srcH, int srcW,
    int dstH, int dstW
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= dstW || y >= dstH) return;

  const float fx = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f;
  const float fy = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;
  const int srcPlane = srcH * srcW;
  const int dstPlane = dstH * dstW;
  const int idx = y * dstW + x;
  float r = sample_plane_normalized(in_rgb_chw + 0 * srcPlane, srcH, srcW, dstH, dstW, fy, fx);
  float g = sample_plane_normalized(in_rgb_chw + 1 * srcPlane, srcH, srcW, dstH, dstW, fy, fx);
  float b = sample_plane_normalized(in_rgb_chw + 2 * srcPlane, srcH, srcW, dstH, dstW, fy, fx);
  out_rgb_chw[0 * dstPlane + idx] = fminf(fmaxf(r, 0.0f), 1.0f);
  out_rgb_chw[1 * dstPlane + idx] = fminf(fmaxf(g, 0.0f), 1.0f);
  out_rgb_chw[2 * dstPlane + idx] = fminf(fmaxf(b, 0.0f), 1.0f);
}

void launch_nv12_to_rgb_chw_clamped(
    const void* y_plane, const void* uv_plane,
    void* out_rgb_chw_fp32,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int input_format,
    int dstH, int dstW,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((dstW + block.x - 1) / block.x, (dstH + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  nv12_to_rgb_chw_clamped_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)y_plane, (const uint8_t*)uv_plane,
      (float*)out_rgb_chw_fp32,
      srcH, srcW, y_pitch, uv_pitch, input_format,
      dstH, dstW
  );
}

void launch_rgba8_to_rgb_chw_resampled(
    const void* in_rgba,
    void* out_rgb_chw_fp32,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((dstW + block.x - 1) / block.x, (dstH + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgba8_to_rgb_chw_resampled_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (float*)out_rgb_chw_fp32,
      srcH, srcW, dstH, dstW
  );
}

void launch_rgba8_to_rgba8_resampled(
    const void* in_rgba,
    void* out_rgba,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((dstW + block.x - 1) / block.x, (dstH + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgba8_to_rgba8_resampled_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_rgba,
      srcH, srcW, dstH, dstW
  );
}

void launch_rgb_chw_to_rgb_chw_resampled(
    const void* in_rgb_chw_fp32,
    void* out_rgb_chw_fp32,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((dstW + block.x - 1) / block.x, (dstH + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgb_chw_to_rgb_chw_resampled_kernel<<<grid, block, 0, s>>>(
      (const float*)in_rgb_chw_fp32, (float*)out_rgb_chw_fp32,
      srcH, srcW, dstH, dstW
  );
}

void launch_rgb_chw_to_rgba8_resampled(
    const void* in_rgb_chw_fp32,
    void* out_rgba,
    int srcH, int srcW,
    int dstH, int dstW,
    void* stream
) {
  dim3 block(32, 8);
  dim3 grid((dstW + block.x - 1) / block.x, (dstH + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  rgb_chw_to_rgba8_resampled_kernel<<<grid, block, 0, s>>>(
      (const float*)in_rgb_chw_fp32, (uint8_t*)out_rgba,
      srcH, srcW, dstH, dstW
  );
}
