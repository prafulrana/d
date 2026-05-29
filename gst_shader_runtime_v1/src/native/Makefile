NVCC ?= /usr/local/cuda/bin/nvcc
CC ?= gcc
CXX ?= g++

NVVFX_LIB_DIR ?= nvvfx/libs

NVCC_GENCODE := \
  -gencode arch=compute_86,code=sm_86 \
  -gencode arch=compute_89,code=sm_89 \
  -gencode arch=compute_90,code=sm_90 \
  -gencode arch=compute_120,code=sm_120 \
  -gencode arch=compute_90,code=compute_90

CFLAGS := -O3 -Wall -Wno-unused-result -fPIC \
  -I/usr/local/cuda/include \
  -Imaxine-audio/nvafx/include \
  -Imaxine-audio/features/dereverb_denoiser/include \
  -Imaxine-audio/features/superres/include \
  -I.

CXXFLAGS := -O3 -Wall -fPIC -std=c++17 \
  -I/usr/local/cuda/include \
  -I.

LDLIBS := \
  -L/usr/local/cuda/lib64/stubs -lcuda \
  -L/usr/local/cuda/lib64 -lcudart \
  -L../cuda -lfilters \
  -L$(NVVFX_LIB_DIR) -lVideoFX -lNVCVImage \
  -Lmaxine-audio/nvafx/lib -lnv_audiofx \
  -lnvinfer -lnvinfer_plugin \
  -lavformat -lavcodec -lavutil -lswresample \
  -lm -lpthread -ldl -lstdc++

LDFLAGS := \
  -Wl,--disable-new-dtags \
  -Wl,-rpath,/opt/dgst/src/native/$(NVVFX_LIB_DIR) \
  -Wl,-rpath,/opt/dgst/src/cuda \
  -Wl,-rpath,/opt/dgst/src/native/maxine-audio/nvafx/lib \
  -Wl,-rpath,/opt/dgst/src/native/maxine-audio/features/dereverb_denoiser/lib \
  -Wl,-rpath,/opt/dgst/src/native/maxine-audio/features/superres/lib \
  -Wl,-rpath,/usr/local/cuda/lib64

all: ../cuda/libfilters.so ../../d_native_processor

CUDA_FILTER_MODULES := $(wildcard ../cuda/filters/*.inc.cu)
RUNTIME_MODULES := $(wildcard runtime/*.inc.c)
PIPELINE_MANIFEST_MODULES := $(wildcard pipeline_manifest.d/*.inc.c)
PIPELINE_STAGE_MODULES := $(wildcard pipeline_stages/*.inc.c)

../cuda/libfilters.so: ../cuda/filters.cu $(CUDA_FILTER_MODULES)
	$(NVCC) -O3 --use_fast_math $(NVCC_GENCODE) -Xcompiler -fPIC -shared \
	  ../cuda/filters.cu -o ../cuda/libfilters.so

trt_sr_engine.o: trt_sr_engine.cpp trt_sr_engine.h
	$(CXX) $(CXXFLAGS) -c trt_sr_engine.cpp -o trt_sr_engine.o

pipeline_manifest.o: pipeline_manifest.c $(PIPELINE_MANIFEST_MODULES) pipeline_manifest.h jsmn.h
	$(CC) $(CFLAGS) -c pipeline_manifest.c -o pipeline_manifest.o

pipeline_stages.o: pipeline_stages.c $(PIPELINE_STAGE_MODULES) bake_internal.h bake.h pipeline_manifest.h perf_ring.h
	$(CC) $(CFLAGS) -c pipeline_stages.c -o pipeline_stages.o

bake_runtime.o: bake_runtime.c $(RUNTIME_MODULES) bake_internal.h bake.h pipeline_manifest.h perf_ring.h trt_sr_engine.h
	$(CC) $(CFLAGS) -c bake_runtime.c -o bake_runtime.o

../../d_native_processor: bake.c bake.h pipeline_manifest.h pipeline_manifest.o pipeline_stages.o bake_runtime.o trt_sr_engine.h trt_sr_engine.o jsmn.h ../cuda/libfilters.so
	$(CC) $(CFLAGS) bake.c bake_runtime.o pipeline_manifest.o pipeline_stages.o trt_sr_engine.o -o ../../d_native_processor $(LDFLAGS) $(LDLIBS)

clean:
	rm -f ../../d_native_processor trt_sr_engine.o pipeline_manifest.o pipeline_stages.o bake_runtime.o ../cuda/libfilters.so

.PHONY: all clean
