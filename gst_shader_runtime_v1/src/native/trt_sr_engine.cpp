// TensorRT super-resolution runner (C-linkage wrapper).
//
// Single execution context, fixed shape, owns the deserialized engine and
// a device workspace. Bindings are set per-run from caller-supplied device
// pointers so we don't have to copy or own input/output buffers.
//
// Built with the libnvinfer / libnvinfer_plugin supplied by the consumer's
// CUDA/TensorRT runtime image.

#include "trt_sr_engine.h"

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <NvInferRuntime.h>

#include <cuda_runtime_api.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

class Logger : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::fprintf(stderr, "[trt_sr_engine] %s\n", msg);
        }
    }
};

Logger g_logger;

struct Trt {
    nvinfer1::IRuntime*          runtime  = nullptr;
    nvinfer1::ICudaEngine*       engine   = nullptr;
    nvinfer1::IExecutionContext* context  = nullptr;
    CUstream                     stream   = nullptr;
    int                          in_w     = 0;
    int                          in_h     = 0;
    int                          out_w    = 0;
    int                          out_h    = 0;
    const char*                  in_name  = nullptr;
    const char*                  out_name = nullptr;
    size_t                       devmem   = 0;
};

void copy_err(char err[256], const char* s) {
    if (!err) return;
    std::snprintf(err, 256, "%s", s ? s : "unknown");
}

const char* dtype_name(nvinfer1::DataType t) {
    switch (t) {
        case nvinfer1::DataType::kFLOAT: return "float32";
        case nvinfer1::DataType::kHALF: return "float16";
        case nvinfer1::DataType::kINT8: return "int8";
        case nvinfer1::DataType::kINT32: return "int32";
        case nvinfer1::DataType::kBOOL: return "bool";
        case nvinfer1::DataType::kUINT8: return "uint8";
        case nvinfer1::DataType::kFP8: return "fp8";
        default: return "unknown";
    }
}

const char* format_name(nvinfer1::TensorFormat f) {
    switch (f) {
        case nvinfer1::TensorFormat::kLINEAR: return "linear";
        case nvinfer1::TensorFormat::kCHW2: return "chw2";
        case nvinfer1::TensorFormat::kHWC8: return "hwc8";
        case nvinfer1::TensorFormat::kCHW4: return "chw4";
        case nvinfer1::TensorFormat::kCHW16: return "chw16";
        case nvinfer1::TensorFormat::kCHW32: return "chw32";
        case nvinfer1::TensorFormat::kDHWC8: return "dhwc8";
        case nvinfer1::TensorFormat::kCDHW32: return "cdhw32";
        case nvinfer1::TensorFormat::kHWC: return "hwc";
        case nvinfer1::TensorFormat::kDLA_LINEAR: return "dla_linear";
        case nvinfer1::TensorFormat::kDLA_HWC4: return "dla_hwc4";
        case nvinfer1::TensorFormat::kHWC16: return "hwc16";
        default: return "unknown";
    }
}

} // namespace

extern "C" {

TrtSrEngine* trt_sr_engine_create(const char* engine_path,
                               int in_w, int in_h,
                               int out_w, int out_h,
                               CUstream stream,
                               char err[256]) {
    if (!engine_path || !*engine_path) { copy_err(err, "engine_path_required"); return nullptr; }

    std::ifstream f(engine_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) { copy_err(err, "engine_open_failed"); return nullptr; }
    const std::streamsize bytes = f.tellg();
    if (bytes <= 0) { copy_err(err, "engine_empty"); return nullptr; }
    f.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(bytes));
    if (!f.read(buf.data(), bytes)) { copy_err(err, "engine_read_failed"); return nullptr; }
    f.close();

    auto t = new Trt();
    t->stream = stream;
    t->in_w = in_w;  t->in_h = in_h;
    t->out_w = out_w; t->out_h = out_h;

    if (!initLibNvInferPlugins(&g_logger, "")) {
        copy_err(err, "initLibNvInferPlugins_failed");
        delete t;
        return nullptr;
    }

    t->runtime = nvinfer1::createInferRuntime(g_logger);
    if (!t->runtime) { copy_err(err, "createInferRuntime_failed"); delete t; return nullptr; }

    t->engine = t->runtime->deserializeCudaEngine(buf.data(), buf.size());
    if (!t->engine) {
        copy_err(err, "deserialize_failed");
        delete t->runtime; delete t; return nullptr;
    }

    // Discover I/O tensor names. SRVGGNetCompact ONNX export produced exactly
    // one input ("input") and one output ("output") — but we look them up by
    // I/O direction so we're robust to renames.
    const int nbIo = t->engine->getNbIOTensors();
    if (nbIo != 2) {
        char buf2[256];
        std::snprintf(buf2, sizeof(buf2), "io_tensor_count_mismatch got=%d want=2", nbIo);
        copy_err(err, buf2);
        delete t->engine; delete t->runtime; delete t; return nullptr;
    }
    int input_count = 0;
    int output_count = 0;
    for (int i = 0; i < nbIo; ++i) {
        const char* name = t->engine->getIOTensorName(i);
        if (t->engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
            t->in_name = name;
            input_count++;
        } else {
            t->out_name = name;
            output_count++;
        }
    }
    if (!t->in_name || !t->out_name || input_count != 1 || output_count != 1) {
        copy_err(err, "io_tensor_lookup_failed");
        delete t->engine; delete t->runtime; delete t; return nullptr;
    }

    // Verify shape, dtype, and memory format match the CUDA bridge contract.
    // The engines may run FP16 internally, but their public I/O must remain
    // float32 NCHW linear because filters/40_trt_bridge writes and reads that
    // exact layout in device memory. Anything else needs explicit conversion
    // kernels and must not be guessed here.
    nvinfer1::Dims din  = t->engine->getTensorShape(t->in_name);
    nvinfer1::Dims dout = t->engine->getTensorShape(t->out_name);
    auto bad = [&](const char* w, const nvinfer1::Dims& d, int eW, int eH) {
        if (d.nbDims != 4 || d.d[0] != 1 || d.d[1] != 3 || d.d[2] != eH || d.d[3] != eW) {
            char buf2[256];
            std::snprintf(buf2, sizeof(buf2),
                          "%s_dim_mismatch got=[%d,%d,%d,%d] want=[1,3,%d,%d]",
                          w, d.nbDims > 0 ? static_cast<int>(d.d[0]) : -1,
                          d.nbDims > 1 ? static_cast<int>(d.d[1]) : -1,
                          d.nbDims > 2 ? static_cast<int>(d.d[2]) : -1,
                          d.nbDims > 3 ? static_cast<int>(d.d[3]) : -1,
                          eH, eW);
            copy_err(err, buf2);
            return true;
        }
        return false;
    };
    if (bad("input", din, in_w, in_h) || bad("output", dout, out_w, out_h)) {
        delete t->engine; delete t->runtime; delete t; return nullptr;
    }
    const nvinfer1::DataType in_dtype = t->engine->getTensorDataType(t->in_name);
    const nvinfer1::DataType out_dtype = t->engine->getTensorDataType(t->out_name);
    const nvinfer1::TensorFormat in_format = t->engine->getTensorFormat(t->in_name);
    const nvinfer1::TensorFormat out_format = t->engine->getTensorFormat(t->out_name);
    if (in_dtype != nvinfer1::DataType::kFLOAT || out_dtype != nvinfer1::DataType::kFLOAT ||
        in_format != nvinfer1::TensorFormat::kLINEAR || out_format != nvinfer1::TensorFormat::kLINEAR) {
        char buf2[256];
        std::snprintf(buf2, sizeof(buf2),
                      "io_contract_mismatch in=%s/%s out=%s/%s want=float32/linear",
                      dtype_name(in_dtype), format_name(in_format),
                      dtype_name(out_dtype), format_name(out_format));
        copy_err(err, buf2);
        delete t->engine; delete t->runtime; delete t; return nullptr;
    }

    t->context = t->engine->createExecutionContext();
    if (!t->context) {
        copy_err(err, "createExecutionContext_failed");
        delete t->engine; delete t->runtime; delete t; return nullptr;
    }

    // Defensive: even for static-shape engines, TRT 10.x's enqueueV3 path
    // expects input shape to be set on the context. Set it once at create.
    nvinfer1::Dims4 in_shape{1, 3, in_h, in_w};
    if (!t->context->setInputShape(t->in_name, in_shape)) {
        copy_err(err, "setInputShape_failed");
        delete t->context; delete t->engine; delete t->runtime; delete t; return nullptr;
    }
    // Pin TRT to our single bake stream — pass a zero-length aux-stream array.
    // Without this, TRT 10.x can fan out kernels onto auxiliary streams that
    // are NOT ordered with our downstream resample/blend kernels enqueued on
    // the bake stream. That race was the source of the "first frame black"
    // symptom AND forced us into a per-frame host-side stream barrier after
    // enqueueV3 (which dropped throughput by stalling the CPU mid-pipeline).
    // With aux streams disabled, same-stream ordering does the right thing
    // and we get full GPU pipelining without per-frame CPU stalls.
    cudaStream_t no_aux[1] = { nullptr };
    t->context->setAuxStreams(no_aux, 0);

    t->devmem = t->engine->getDeviceMemorySizeV2();
    std::fprintf(stderr,
                 "[trt_sr_engine] loaded %s in=%dx%d out=%dx%d io=input:%s/%s output:%s/%s devmem=%zu enqueue=V3 aux_streams=0\n",
                 engine_path, in_w, in_h, out_w, out_h,
                 dtype_name(in_dtype), format_name(in_format),
                 dtype_name(out_dtype), format_name(out_format),
                 t->devmem);

    return reinterpret_cast<TrtSrEngine*>(t);
}

void trt_sr_engine_destroy(TrtSrEngine* r) {
    if (!r) return;
    auto t = reinterpret_cast<Trt*>(r);
    if (t->context) { delete t->context; t->context = nullptr; }
    if (t->engine)  { delete t->engine;  t->engine  = nullptr; }
    if (t->runtime) { delete t->runtime; t->runtime = nullptr; }
    delete t;
}

int trt_sr_engine_run(TrtSrEngine* r,
                    CUdeviceptr in_rgb_fp32_nchw,
                    CUdeviceptr out_rgb_fp32_nchw,
                    char err[256]) {
    if (!r) { copy_err(err, "null_runner"); return -1; }
    auto t = reinterpret_cast<Trt*>(r);
    if (!in_rgb_fp32_nchw || !out_rgb_fp32_nchw) {
        copy_err(err, "binding_ptr_null");
        return -1;
    }
    if (!t->context->setTensorAddress(t->in_name,
                                      reinterpret_cast<void*>(static_cast<uintptr_t>(in_rgb_fp32_nchw)))) {
        copy_err(err, "setTensorAddress_in_failed");
        return -1;
    }
    if (!t->context->setTensorAddress(t->out_name,
                                      reinterpret_cast<void*>(static_cast<uintptr_t>(out_rgb_fp32_nchw)))) {
        copy_err(err, "setTensorAddress_out_failed");
        return -1;
    }
    if (!t->context->enqueueV3(t->stream)) {
        copy_err(err, "enqueueV3_failed");
        return -1;
    }
    // No host-side stream barrier here: aux streams are disabled in
    // trt_sr_engine_create, so TRT runs only on t->stream and same-stream ordering
    // ensures the downstream resample/blend kernels see the finished output.
    // Keeping CPU non-blocking is what unblocks the per-frame pipeline.
    cudaError_t cuErr = cudaPeekAtLastError();
    if (cuErr != cudaSuccess) {
        std::snprintf(err, 256, "cuda_after_enqueue=%d:%s",
                      (int)cuErr, cudaGetErrorString(cuErr));
        return -1;
    }
    return 0;
}

size_t trt_sr_engine_device_memory_bytes(const TrtSrEngine* r) {
    if (!r) return 0;
    return reinterpret_cast<const Trt*>(r)->devmem;
}

} // extern "C"
