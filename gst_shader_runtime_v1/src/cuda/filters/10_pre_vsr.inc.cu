// ----------------------------------------------------------------------------
// Native bake pre-kernel. CUDA YUV420 decoder frame in, VSR RGBA8 out.
// Keep subjective filters after VSR, but remove obvious compression grid
// discontinuities before SR so Maxine does not upscale them into hard 4K stairs.
// ----------------------------------------------------------------------------
__global__ void nv12_pre_vsr_filter(
    const uint8_t* __restrict__ y_plane,
    const uint8_t* __restrict__ uv_plane,
    uint8_t* __restrict__ out_rgba,
    int srcH, int srcW,
    int y_pitch, int uv_pitch,
    int input_format,
    int H, int W,
    float contrast, float saturation, float gamma_inv,
    float cas_strength
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;

  float r_c, g_c, b_c;
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y, x, &r_c, &g_c, &b_c);

  float r00, g00, b00, r01, g01, b01, r02, g02, b02;
  float r10, g10, b10, r12, g12, b12;
  float r20, g20, b20, r21, g21, b21, r22, g22, b22;
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y - 1, x - 1, &r00, &g00, &b00);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y - 1, x    , &r01, &g01, &b01);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y - 1, x + 1, &r02, &g02, &b02);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y    , x - 1, &r10, &g10, &b10);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y    , x + 1, &r12, &g12, &b12);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y + 1, x - 1, &r20, &g20, &b20);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y + 1, x    , &r21, &g21, &b21);
  yuv420_rgb_at(y_plane, uv_plane, srcH, srcW, y_pitch, uv_pitch, H, W, input_format, y + 1, x + 1, &r22, &g22, &b22);

  float r_blur = (r00 + 2.0f*r01 + r02 + 2.0f*r10 + 4.0f*r_c + 2.0f*r12 + r20 + 2.0f*r21 + r22) * (1.0f / 16.0f);
  float g_blur = (g00 + 2.0f*g01 + g02 + 2.0f*g10 + 4.0f*g_c + 2.0f*g12 + g20 + 2.0f*g21 + g22) * (1.0f / 16.0f);
  float b_blur = (b00 + 2.0f*b01 + b02 + 2.0f*b10 + 4.0f*b_c + 2.0f*b12 + b20 + 2.0f*b21 + b22) * (1.0f / 16.0f);

  float r = r_blur + (r_c - r_blur) * cas_strength;
  float g = g_blur + (g_c - g_blur) * cas_strength;
  float b = b_blur + (b_c - b_blur) * cas_strength;

  if (gamma_inv != 1.0f) {
    r = powf(fmaxf(r, 1e-6f), gamma_inv);
    g = powf(fmaxf(g, 1e-6f), gamma_inv);
    b = powf(fmaxf(b, 1e-6f), gamma_inv);
  }
  if (contrast != 1.0f) {
    r = (r - 0.5f) * contrast + 0.5f;
    g = (g - 0.5f) * contrast + 0.5f;
    b = (b - 0.5f) * contrast + 0.5f;
  }
  if (saturation != 1.0f) {
    float luma = luma_bt709(r, g, b);
    r = luma + (r - luma) * saturation;
    g = luma + (g - luma) * saturation;
    b = luma + (b - luma) * saturation;
  }

  const int out_idx = (y * W + x) * 4;
  out_rgba[out_idx + 0] = clamp01_to_u8(r);
  out_rgba[out_idx + 1] = clamp01_to_u8(g);
  out_rgba[out_idx + 2] = clamp01_to_u8(b);
  out_rgba[out_idx + 3] = 255;
}

