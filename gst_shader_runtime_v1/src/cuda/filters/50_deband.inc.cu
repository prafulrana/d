// ----------------------------------------------------------------------------
// DEBAND 4K (kills gradient banding in skies/walls/dark scenes).
//
// Per-pixel: compute local luma gradient magnitude over 3x3. If the gradient
// is small AND luma is low-to-mid (where banding is most visible to the eye),
// add a small dithered jitter to the RGB to break the visible band steps.
// Real edges (high gradient) pass through unchanged.
//
// Strength is keyed to luma — most aggressive in the 5%–60% luma window where
// 8-bit gradients show banding. Cost: ~1.5 ms at 4K.
// ----------------------------------------------------------------------------
__device__ __forceinline__ uint32_t hash3(uint32_t x, uint32_t y, uint32_t seed) {
  uint32_t h = x * 0x27d4eb2du + y * 0x165667b1u + seed * 0x9e3779b1u;
  h ^= h >> 15; h *= 0x85ebca77u;
  h ^= h >> 13; h *= 0xc2b2ae3du;
  h ^= h >> 16;
  return h;
}

__global__ void deband_4k_rgba_kernel(
    const uint8_t* __restrict__ in_rgba,
    uint8_t* __restrict__ out_rgba,
    int H, int W,
    float strength, uint32_t frame_seed
) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= W || y >= H) return;
  const int idx = ((size_t)y * W + x) * 4;
  // Sample 3x3 luma neighborhood from input.
  int xm = x > 0 ? x - 1 : x;
  int xp = x + 1 < W ? x + 1 : x;
  int ym = y > 0 ? y - 1 : y;
  int yp = y + 1 < H ? y + 1 : y;
  #define LUMA_AT(rx, ry) ({ \
    const uint8_t* p = in_rgba + ((size_t)(ry) * W + (rx)) * 4; \
    (0.2126f * (float)p[0] + 0.7152f * (float)p[1] + 0.0722f * (float)p[2]); \
  })
  float L00 = LUMA_AT(xm, ym), L01 = LUMA_AT(x, ym), L02 = LUMA_AT(xp, ym);
  float L10 = LUMA_AT(xm, y ), L11 = LUMA_AT(x, y ), L12 = LUMA_AT(xp, y );
  float L20 = LUMA_AT(xm, yp), L21 = LUMA_AT(x, yp), L22 = LUMA_AT(xp, yp);
  #undef LUMA_AT
  // Sobel gradient magnitude (in 0..255 luma units).
  float gx = (L02 + 2.0f*L12 + L22) - (L00 + 2.0f*L10 + L20);
  float gy = (L20 + 2.0f*L21 + L22) - (L00 + 2.0f*L01 + L02);
  float gmag = sqrtf(gx*gx + gy*gy) * 0.125f; // ~normalized 0..32

  // Flatness mask: 1 when gradient is small. Smooth fall-off to avoid hard
  // transitions at edges.
  float flat = 1.0f - fminf(gmag * (1.0f/6.0f), 1.0f);
  // Luma weight: most visible in 5..60 luma window (banding kicks in dark→mid).
  float L = L11 * (1.0f/255.0f);
  float lwt = (L > 0.02f && L < 0.65f) ? 1.0f : 0.3f;
  float amp = strength * flat * lwt;

  // Hash-based dither in [-1, +1] luma units (scaled by amp). Different per
  // pixel + per frame to avoid static-pattern artifacts.
  uint32_t h = hash3((uint32_t)x, (uint32_t)y, frame_seed);
  float dith = ((float)(h >> 24) - 127.5f) * (1.0f / 127.5f); // [-1, +1]
  float jitter = dith * amp; // luma units in 0..255 scale

  // Apply dither to all 3 channels (same luma offset → preserves hue).
  float r = (float)in_rgba[idx + 0] + jitter;
  float g = (float)in_rgba[idx + 1] + jitter;
  float b = (float)in_rgba[idx + 2] + jitter;
  if (r < 0.0f) r = 0.0f; if (r > 255.0f) r = 255.0f;
  if (g < 0.0f) g = 0.0f; if (g > 255.0f) g = 255.0f;
  if (b < 0.0f) b = 0.0f; if (b > 255.0f) b = 255.0f;
  out_rgba[idx + 0] = (uint8_t)(r + 0.5f);
  out_rgba[idx + 1] = (uint8_t)(g + 0.5f);
  out_rgba[idx + 2] = (uint8_t)(b + 0.5f);
  out_rgba[idx + 3] = 255;
}

void launch_deband_4k_rgba(
    const void* in_rgba, void* out_rgba,
    int H, int W, float strength, uint32_t frame_seed, void* stream
) {
  dim3 block(32, 8);
  dim3 grid((W + block.x - 1) / block.x, (H + block.y - 1) / block.y);
  cudaStream_t s = (cudaStream_t)stream;
  deband_4k_rgba_kernel<<<grid, block, 0, s>>>(
      (const uint8_t*)in_rgba, (uint8_t*)out_rgba,
      H, W, strength, frame_seed
  );
}

