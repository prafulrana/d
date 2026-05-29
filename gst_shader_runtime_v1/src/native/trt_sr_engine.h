/* C-linkage wrapper around a TensorRT super-resolution engine.
 *
 * Loads the .engine file at create, holds a single IExecutionContext,
 * runs inference on a fixed-shape input (1×3×H_in×W_in float) producing
 * a fixed-shape output (1×3×H_out×W_out float). Caller owns the device
 * pointers for both input and output buffers — we wire them as bindings
 * and call enqueueV3 on the supplied CUDA stream.
 *
 * Per-frame CPU work is only binding device addresses and enqueueing V3 on
 * the supplied stream. The caller-owned buffers are CUDA device pointers;
 * no host tensor buffer or CPU fallback path exists here.
 */
#ifndef DPROC_TRT_SR_ENGINE_H
#define DPROC_TRT_SR_ENGINE_H

#include <cuda.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TrtSrEngine TrtSrEngine;

/* Create a runner from a serialized .engine file.
 *   engine_path : path to the trtexec --saveEngine output
 *   in_w, in_h  : expected input dims (must match the engine)
 *   out_w, out_h: expected output dims (must match the engine)
 *   stream      : CUDA stream that run() will enqueue on
 *   err         : 256-byte buffer for human-readable error
 * Returns NULL on failure (err populated). */
TrtSrEngine* trt_sr_engine_create(const char* engine_path,
                               int in_w, int in_h,
                               int out_w, int out_h,
                               CUstream stream,
                               char err[256]);

void trt_sr_engine_destroy(TrtSrEngine* r);

/* Async on the stream passed at create.
 *   in_rgb_fp32_nchw : device ptr, layout (1,3,in_h,in_w) float32, row-major
 *   out_rgb_fp32_nchw: device ptr, layout (1,3,out_h,out_w) float32, row-major
 * Returns 0 on success, -1 with err populated on failure. */
int trt_sr_engine_run(TrtSrEngine* r,
                    CUdeviceptr in_rgb_fp32_nchw,
                    CUdeviceptr out_rgb_fp32_nchw,
                    char err[256]);

/* Query the device-memory footprint we asked TRT to allocate. */
size_t trt_sr_engine_device_memory_bytes(const TrtSrEngine* r);

#ifdef __cplusplus
}
#endif

#endif /* DPROC_TRT_SR_ENGINE_H */
