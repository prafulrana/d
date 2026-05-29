/*
Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

/* Effect version: 2.1.0 */
#ifndef __linux__
#error "This header is only supported on linux."
#endif /* __linux__ */

#ifndef __NVAUDIOEFFECTS_DEREVERB_DENOISER_H__
#define __NVAUDIOEFFECTS_DEREVERB_DENOISER_H__

#if defined(__cplusplus)
extern "C" {
#endif

/** Effect selector. @ref NvAFX_EffectSelector */
/** Combined De-reverb and Denoiser Effects */
#define NVAFX_EFFECT_DEREVERB_DENOISER "dereverb_denoiser"

#if defined(__cplusplus)
}
#endif

#endif  /* __NVAUDIOEFFECTS_DEREVERB_DENOISER_H__ */
