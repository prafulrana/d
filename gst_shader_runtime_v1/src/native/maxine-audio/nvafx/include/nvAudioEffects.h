/*
Copyright (c) 2020-2025, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

/* AUDIOEFFECTS_VERSION_2.0.0 */
#ifndef __linux__
#error "This header is only supported on linux."
#endif /* __linux__ */

#ifndef __NVAUDIOEFFECTS_H__
#define __NVAUDIOEFFECTS_H__

#if defined(__cplusplus)
extern "C" {
#endif

/** API return values */
typedef enum {
  /** Success */
  NVAFX_STATUS_SUCCESS = 0,
  /** Failure */
  NVAFX_STATUS_FAILED = 1,
  /** Handle invalid */
  NVAFX_STATUS_INVALID_HANDLE = 2,
  /** Parameter value invalid */
  NVAFX_STATUS_INVALID_PARAM = 3,
  /** Parameter value immutable */
  NVAFX_STATUS_IMMUTABLE_PARAM = 4,
  /** Insufficient data to process */
  NVAFX_STATUS_INSUFFICIENT_DATA = 5,
  /** Effect not supported */
  NVAFX_STATUS_EFFECT_NOT_AVAILABLE = 6,
  /** Given buffer length too small to hold requested data */
  NVAFX_STATUS_OUTPUT_BUFFER_TOO_SMALL = 7,
  /** Model file could not be loaded */
  NVAFX_STATUS_MODEL_LOAD_FAILED = 8,
  /** Model is not loaded, it needs to be loaded for this operation */
  NVAFX_STATUS_MODEL_NOT_LOADED = 9,
  /** Selected model is incompatible */
  NVAFX_STATUS_INCOMPATIBLE_MODEL = 10,
  /** The selected GPU is not supported. The SDK requires Turing and above GPU with Tensor cores */
  NVAFX_STATUS_GPU_UNSUPPORTED = 11,
  /** No supported GPU found on the system */
  NVAFX_STATUS_NO_SUPPORTED_GPU_FOUND = 12,
  /** Current GPU is not the one selected */
  NVAFX_STATUS_WRONG_GPU = 13,
  /** Cuda operation failure */
  NVAFX_STATUS_CUDA_ERROR = 14,
  /** Invalid operation performed **/
  NVAFX_STATUS_INVALID_OPERATION = 15,
  /** CUDA runtime is less than supported version*/
  NVAFX_UNSUPPORTED_RUNTIME = 16,
} NvAFX_Status;

/** Bool type (stdbool is available only with C99) */
#define NVAFX_TRUE 1
#define NVAFX_FALSE 0
typedef char NvAFX_Bool;

/** Minimum supported cuda runtime is 12.8 */
#define CUDA_SUPPORTED_RUNTIME 12080

/** Logging level to enable, each level is inclusive of the level preceding it */
typedef enum LoggingSeverity_t {
  LOG_LEVEL_ERROR,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_INFO,
} LoggingSeverity;

typedef enum LoggingTarget_t {
  // No logging.
  LOG_TARGET_NONE = 0x0,
  // Log to stderr.
  LOG_TARGET_STDERR = 0x1,
  // Log to specified file.
  LOG_TARGET_FILE = 0x2,
  // Log through invocation of a user-specified callback.
  LOG_TARGET_CALLBACK = 0x4,
} LoggingTarget;

inline const char* LogSeverityToString(LoggingSeverity severity) {
  switch (severity) {
    case LOG_LEVEL_ERROR:    return "ERROR";
    case LOG_LEVEL_WARNING:  return "WARNING";
    case LOG_LEVEL_INFO:     return "INFO";
    default: return "UNKNOWN";
  }
}

/** Function used for logging callback */
typedef void(*logging_cb_t)(LoggingSeverity level, const char* log, void* userdata);

/** We use strings as effect selectors */
typedef const char* NvAFX_EffectSelector;

/** We use strings as parameter selectors. */
typedef const char* NvAFX_ParameterSelector;

/** Each effect instantiation is associated with an opaque handle. */
typedef void* NvAFX_Handle;

NvAFX_Status NvAFX_CreateEffect(NvAFX_EffectSelector code, NvAFX_Handle* effect);

/** @brief Create a new instance of a chained audio effect.
 *
 * @param[in] code   The selector code for the desired chain.
 * @param[out] effect  A handle to the created effect instance.
 */
NvAFX_Status NvAFX_CreateChainedEffect(NvAFX_EffectSelector code, NvAFX_Handle* effect);

/** @brief Delete a previously instantiated audio Effect.
 *
 * @param[in]  effect A handle to the audio Effect to be deleted.
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_DestroyEffect(NvAFX_Handle effect);

/** Set the value of the selected parameter (unsigned int, char*, float)
 *
 * @param[in]  effect      The effect to configure.
 * @param[in]  param_name  The selector of the effect parameter to configure.
 * @param[in]  val         The value to be assigned to the selected effect parameter.
 * @param[in]  list[]      The list with values to be assigned to the selected effect parameter.
 * @param[in]  list_size   Number of elements of the list[]

 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_SetBool(NvAFX_Handle effect, NvAFX_ParameterSelector param_name, NvAFX_Bool val);
NvAFX_Status NvAFX_SetBoolList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                               const NvAFX_Bool* list, unsigned int list_size);
NvAFX_Status NvAFX_SetU32(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                          unsigned int val);
NvAFX_Status NvAFX_SetU32List(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                              unsigned int* list, unsigned int list_size);
NvAFX_Status NvAFX_SetFloat(NvAFX_Handle effect, NvAFX_ParameterSelector param_name, float val);
NvAFX_Status NvAFX_SetFloatList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                                float* list, unsigned int list_size);
NvAFX_Status NvAFX_SetString(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                             const char* val);
NvAFX_Status NvAFX_SetStringList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                                 const char** list, unsigned int list_size);

/** Set the value of the selected parameter for specific streams
 *
 * @param[in]  effect        The effect to configure.
 * @param[in]  param_name    The selector of the effect parameter to configure.
 * @param[in]  stream_idx[]  The streams for which values are to be set. If the value of the
 *                           i-th stream is to be set, this array should contain i.
 * @param[in]  values[][]    The values to be set for corresponding stream.
 * @param[in]  list_size     Number of streams specified in stream_idx.

 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_SetStreamFloatList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                                      const unsigned int* stream_idx, const float** values,
                                      unsigned int list_size);

/** Get the value of the selected parameter (unsigned int, char*, float)
*
* @param[in]  effect       The effect handle.
* @param[in]  param_name   The selector of the effect parameter to read.
* @param[out] val          Buffer in which the parameter value will be assigned.
* @param[in]  max_length  The length in bytes of the buffer provided.
* @param[out] list[]      The list with values to be retrieved for the selected effect parameter.
* @param[out] list_size   Number of elements of list[]
*
* @return Status values as enumerated in @ref NvAFX_Status
*/
NvAFX_Status NvAFX_GetBool(NvAFX_Handle effect, NvAFX_ParameterSelector param_name, NvAFX_Bool* val);
NvAFX_Status NvAFX_GetBoolList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                               NvAFX_Bool* list, unsigned int* list_size);
NvAFX_Status NvAFX_GetU32(NvAFX_Handle effect, NvAFX_ParameterSelector param_name, unsigned int* val);
NvAFX_Status NvAFX_GetU32List(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                              unsigned int* list, unsigned int* list_size);
NvAFX_Status NvAFX_GetFloat(NvAFX_Handle effect, NvAFX_ParameterSelector param_name, float* val);
NvAFX_Status NvAFX_GetFloatList(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                                float* list, unsigned int* list_size);
NvAFX_Status NvAFX_GetString(NvAFX_Handle effect, NvAFX_ParameterSelector param_name,
                             char* val, int max_length);

/** Load the Effect based on the set params.
 *
 * @param[in]  effect     The effect object handle.
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_Load(NvAFX_Handle effect);

/** Process the input buffer as per the effect selected. e.g. denoising
 *
 * @note The input float data is expected to be standard 32-bit float type with values in range [-1.0, +1.0]
 *
 * @param[in]  effect        The effect handle.
 * @param[in]  input         Input float buffer array. It points to an array of buffers where each buffer holds
 *                           audio data for a single channel. Array size should be same as number of
 *                           input channels expected by the effect. Also ensure sampling rate is same as
 *                           expected by the Effect.
 *                           For e.g. for denoiser it should be equal to the value returned by NvAFX_GetU32()
 *                           returned value for NVAFX_FIXED_PARAM_SAMPLE_RATE parameter.
 * @param[out]  output       Output float buffer array. The layout is same as input. It points to an an array of
 *                           buffers where buffer has audio data corresponding to that channel. The buffers have
 *                           to be preallocated by caller. Size of each buffer (i.e. channel) is same as that of
 *                           input. However, number of channels may differ (can be queried by calling
 *                           NvAFX_GetU32() with NVAFX_PARAM_NUM_OUTPUT_CHANNELS as parameter).
 * @param[in]  num_samples   The number of samples in the input buffer. After this call returns output will
 *                           have same number of samples.
 * @param[in]  num_channels  The number of channels in the input buffer. The @a input should point
 *                           to @ num_channels number of buffers for input, which can be determined by
 *                           calling NvAFX_GetU32() with NVAFX_PARAM_NUM_INPUT_CHANNELS as parameter.
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_Run(NvAFX_Handle effect, const float** input, float** output,
                       unsigned num_input_samples, unsigned num_input_channels);


/** Reset effect state
 *
 * @note Allows the state of an effect to be reset. This operation will reset the state of selected in the next
 *       NvAFX_Run call
 *
 * @param[in]  effect        The effect handle.
 * @param[in]  bitmap        Array of NvAFX_Bool parameters which indicates whether a stream is to be reset.
 *                           If the i-th stream is to be reset, the i-th value of the array should be set to
 *                           NVAFX_TRUE, else it should be set to NVAFX_FALSE.
 * @param[in]  length        The length of the array.
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_Reset(NvAFX_Handle effect, NvAFX_Bool* bitmap, int length);

/** Initialize Logger
 *
 * @note Initializes Logger
 *
 * @param[in]  level         The logging level to enable.
 * @param[in]  LoggingTarget Logging targets to write logs to, LoggingTarget_t can be OR'd
 * @param[in]  filename      The name of the file where to write logs.
 * @param[in]  cb            Callback to use if LOG_TARGET_CALLBACK is enabled.
 * @param[in]  userdata      Data passed back with log callback. Used only when LOG_TARGET_CALLBACK
 *                           is enabled.
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_InitializeLogger(LoggingSeverity level, int target,
                                    const char *filename, logging_cb_t cb,
                                    void* userdata);

/** Un-initializes Logger
 *
 * @note Un-initializes Logger
 *
 * @return Status values as enumerated in @ref NvAFX_Status
 */
NvAFX_Status NvAFX_UninitializeLogger();

/** Effect selectors. @ref NvAFX_EffectSelector */


/** Denoiser 16k + Superres 16k to 48k */
#define NVAFX_CHAINED_EFFECT_DENOISER_16k_SUPERRES_16k_TO_48k "denoiser16k_superres16kto48k"
/** De-reverb 16k + Superres 16k to 48k */
#define NVAFX_CHAINED_EFFECT_DEREVERB_16k_SUPERRES_16k_TO_48k "dereverb16k_superres16kto48k"
/** Combined De-reverb and Denoiser 16k + Superres 16k to 48k */
#define NVAFX_CHAINED_EFFECT_DEREVERB_DENOISER_16k_SUPERRES_16k_TO_48k "dereverb_denoiser16k_superres16kto48k"
/** Superres 8k to 16k + Denoiser 16k */
#define NVAFX_CHAINED_EFFECT_SUPERRES_8k_TO_16k_DENOISER_16k "superres8kto16k_denoiser16k"
/** Superres 8k to 16k + De-reverb 16k */
#define NVAFX_CHAINED_EFFECT_SUPERRES_8k_TO_16k_DEREVERB_16k "superres8kto16k_dereverb16k"
/** Superres 8k to 16k + Combined De-reverb and Denoiser 16k */
#define NVAFX_CHAINED_EFFECT_SUPERRES_8k_TO_16k_DEREVERB_DENOISER_16k "superres8kto16k_dereverb_denoiser16k"
/** Speaker Focus 16k + Denoiser 16k */
#define NVAFX_CHAINED_EFFECT_SPEAKER_FOCUS_16k_DENOISER_16k "speaker_focus16k_denoiser16k"
/** Speaker Focus 48k + Denoiser 48k */
#define NVAFX_CHAINED_EFFECT_SPEAKER_FOCUS_48k_DENOISER_48k "speaker_focus48k_denoiser48k"

/** Parameter selectors */

/** Common Effect parameters. @ref NvAFX_ParameterSelector */
/** Stream activity flags for each stream (NvAFX_Bool[]) */
#define NVAFX_PARAM_ACTIVE_STREAMS "active_streams"
/** Scalar defining the strength of the effect (float). Can only range from 0 to 1. */
#define NVAFX_PARAM_INTENSITY_RATIO "intensity_ratio"
/** Model path (char*) for the Effect */
#define NVAFX_PARAM_MODEL_PATH "model_path"
/** Number of input audio channels */
#define NVAFX_PARAM_NUM_INPUT_CHANNELS "num_input_channels"
/** Number of output audio channels */
#define NVAFX_PARAM_NUM_OUTPUT_CHANNELS "num_output_channels"
/** Number of input audio samples per input channel processed in a call to NvAFX_Run */
#define NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME "num_samples_per_input_frame"
/** Number of output audio samples per input channel processed in a call to NvAFX_Run */
#define NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME "num_samples_per_output_frame"
/** Number of reference audio samples per input channel processed in a call to NvAFX_Run */
#define NVAFX_PARAM_NUM_SAMPLES_PER_REFERENCE_FRAME "num_samples_per_reference_frame"
/** Number of audio streams in I/O (unsigned int). */
#define NVAFX_PARAM_NUM_STREAMS "num_streams"
/** Sample rate (unsigned int) of input audio streams.
    Currently supported sample rate(s): 48000, 16000. */
#define NVAFX_PARAM_INPUT_SAMPLE_RATE "input_sample_rate"
/** Output sample rate. */
#define NVAFX_PARAM_OUTPUT_SAMPLE_RATE "output_sample_rate"
/** Supported number of samples per frame (unsigned int*[]) for the effect */
#define NVAFX_PARAM_SUPPORTED_NUM_SAMPLES_PER_FRAME "supported_num_samples_per_frame"
/** To set if SDK should select the default GPU to run the effects in a Multi-GPU setup
    (unsigned int). Default value is 0. Please see user manual for details. */
#define NVAFX_PARAM_USE_DEFAULT_GPU "use_default_gpu"
/** Enable/disable VAD (set 1 to enable, 0 to disable disabled by default) */
#define NVAFX_PARAM_ENABLE_VAD "enable_vad"
/** Voice activity status (boolean). This is immutable parameter */
#define NVAFX_PARAM_VAD_RESULT "vad_result"
/** GPUs to run effect on (chained effects only). Please see user manual for details */
#define NVAFX_PARAM_CHAINED_EFFECT_GPU_LIST "chained_effect_gpu_list"

/** Set effect version.
 *
 *  @note - Effect Version is currently supported only by Denoiser.
 *        - Please refer to the programming guide for further details.
 */
#define NVAFX_PARAM_EFFECT_VERSION "effect_version"

/** Deprecated parameters */
/** Denoiser Model Path v0.5 */
#define NVAFX_PARAM_DENOISER_MODEL_PATH NVAFX_PARAM_MODEL_PATH
/** Denoiser sample rate v0.5 */
#define NVAFX_PARAM_DENOISER_SAMPLE_RATE NVAFX_PARAM_SAMPLE_RATE
/** Denoiser number of samples per frame v0.5 */
#define NVAFX_PARAM_DENOISER_NUM_SAMPLES_PER_FRAME NVAFX_PARAM_NUM_SAMPLES_PER_FRAME
/** Denoiser number of audio channels v0.5 */
#define NVAFX_PARAM_DENOISER_NUM_CHANNELS NVAFX_PARAM_NUM_CHANNELS
/** Denoising factor v0.5 */
#define NVAFX_PARAM_DENOISER_INTENSITY_RATIO NVAFX_PARAM_INTENSITY_RATIO
#define NVAFX_PARAM_DENOISER_SUPPORTED_NUM_SAMPLES_PER_FRAME NVAFX_PARAM_SUPPORTED_NUM_SAMPLES_PER_FRAME
/** Number of audio channels in I/O v1.0*/
#define NVAFX_PARAM_NUM_CHANNELS "num_channels"
/** Number of audio samples processed in a call to NvAFX_Run v1.0 parameter. */
#define NVAFX_PARAM_NUM_SAMPLES_PER_FRAME "num_samples_per_frame"
/** Sample rate (unsigned int) of audio streams v1.0 parameter. */
#define NVAFX_PARAM_SAMPLE_RATE "sample_rate"
/** Studio Mic:  Depreciated. Currently redirects to Studio Voice High Quality */
#define NVAFX_EFFECT_STUDIO_MIC "studio_mic"

#if defined(__cplusplus)
}
#endif

#endif  /* __NVAUDIOEFFECTS_H__ */
