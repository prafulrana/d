#ifndef DPROC_PIPELINE_MANIFEST_H
#define DPROC_PIPELINE_MANIFEST_H

#include <stddef.h>
#include <stdint.h>
#include "jsmn.h"

#define DPROC_MAX_PIPELINE_STAGES 128
#define DPROC_MAX_MODEL_STAGES 16

#define DPROC_MODEL_KIND_UNKNOWN 0
#define DPROC_MODEL_KIND_MODEL   1
#define DPROC_MODEL_KIND_MOTION  2
#define DPROC_MODEL_KIND_CUDA    3

#define DPROC_MODEL_FAMILY_NONE   0
#define DPROC_MODEL_FAMILY_MAXINE 1
#define DPROC_MODEL_FAMILY_ESRGAN 2
#define DPROC_MODEL_FAMILY_CUGAN  3

#define DPROC_MODEL_ENGINE_UNKNOWN 0
#define DPROC_MODEL_ENGINE_TRT     1
#define DPROC_MODEL_ENGINE_MAXINE  2
#define DPROC_MODEL_ENGINE_CUDA    3

typedef struct {
    uint32_t id_hash;
    int      in_w, in_h;
    int      out_w, out_h;
} DProcStage;

typedef struct {
    char id[64];
    char label[128];
    char kind[32];
    char engine[32];
    char op[48];
    char family[32];
    char timing_role[48];
    char pts_policy[48];
    char frame_hold_policy[48];
    char motion_guide[32];
    char engine_path[1024];
    int  kind_code;
    int  engine_code;
    int  family_code;
    int  input_w, input_h, input_fps;
    int  output_w, output_h, output_fps;
    int  infer_fps;
    float motion_strength;
    int  pass_through;
    int  timing_owner;
    int  output_clock_owner;
    int  timing_fps;
    int  ready;
    long long runs;
    long long cadence_dropped;
    long long over_budget;
    double stage_seconds;
} DProcModelStage;

// Execution ABI only: hashes of stage ids from src/pipeline/schema.js.
// UI labels, docs, params, and dimensions stay in that JS schema and the
// worker receives a compact [{id,dims}] plan at spawn time.
#define STAGE_DECODE_NV12        UINT32_C(0xa688b847)
#define STAGE_NV12_TO_RGB_CHW    UINT32_C(0x4af89d95)
#define STAGE_UPSCALER           UINT32_C(0x7d49f0f0)
#define STAGE_RGB_CHW_TO_RGBA8   UINT32_C(0x98b6ffec)
#define STAGE_POST_VSR_FINALIZE  UINT32_C(0xf1dff796)
#define STAGE_DEBAND_4K          UINT32_C(0xc4ed8a7b)
#define STAGE_CUSTOM_SHADER      UINT32_C(0x61b4ca8e)
#define STAGE_DLSAA_TEMPORAL     UINT32_C(0x70de5b6b)
#define STAGE_TEMPORAL_DENOISE   UINT32_C(0x4430118b)
#define STAGE_NVENC_HEVC         UINT32_C(0x75abc6dc)

#define STAGE_AUDIO_DECODE            UINT32_C(0x6b4d64b4)
#define STAGE_AUDIO_TO_16K_MONO       UINT32_C(0x59294936)
#define STAGE_MAXINE_AUDIO_CLEANUP    UINT32_C(0x6081cbfb)
#define STAGE_MAXINE_AUDIO_SUPERRES   UINT32_C(0x4399a6aa)
#define STAGE_AUDIO_EQ_PROFILE        UINT32_C(0xeeabd59a)
#define STAGE_AUDIO_DELAY_SYNC        UINT32_C(0x14b897ed)
#define STAGE_AUDIO_TO_STEREO_48K     UINT32_C(0x1ec57c4a)
#define STAGE_AUDIO_AAC_TRANSPORT     UINT32_C(0xd4aaffef)

int dproc_json_skip_token(const jsmntok_t* toks, int n, int idx);
int dproc_pipeline_manifest_parse(DProcStage stages[DPROC_MAX_PIPELINE_STAGES],
                                 int* stage_count,
                                 const char* json,
                                 char* err,
                                 size_t err_size);
int dproc_model_pipeline_parse(DProcModelStage stages[DPROC_MAX_MODEL_STAGES],
                               int* stage_count,
                               const char* json,
                               char* err,
                               size_t err_size);
int dproc_stage_contains(const DProcStage* stages, int stage_count, uint32_t id);
int dproc_stage_dims_for(const DProcStage* stages, int stage_count, uint32_t id,
                        int* w_out, int* h_out);
const char* dproc_model_family_name(int family_code);
const char* dproc_model_engine_name(int engine_code);

#endif
