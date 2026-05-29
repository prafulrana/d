static void destroy_source_rgba_buffers(BakeWorker* w) {
    if (!w) return;
    if (w->interp_pre_vsr_rgba) {
        cuMemFree(w->interp_pre_vsr_rgba);
        w->interp_pre_vsr_rgba = 0;
    }
    if (w->prev_pre_vsr_rgba) {
        cuMemFree(w->prev_pre_vsr_rgba);
        w->prev_pre_vsr_rgba = 0;
    }
    if (w->pre_vsr_rgba) {
        cuMemFree(w->pre_vsr_rgba);
        w->pre_vsr_rgba = 0;
    }
    w->pre_vsr_rgba_bytes = 0;
}

static void destroy_maxine_effect(BakeWorker* w) {
    if (!w) return;
    if (w->vsr_effect) {
        NvVFX_DestroyEffect(w->vsr_effect);
        w->vsr_effect = NULL;
    }
    w->maxine_input_width = 0;
    w->maxine_input_height = 0;
    memset(&w->vsr_src_im, 0, sizeof(w->vsr_src_im));
    memset(&w->vsr_dst_im, 0, sizeof(w->vsr_dst_im));
}

static void destroy_vsr_effect(BakeWorker* w) {
    if (!w) return;
    destroy_maxine_effect(w);
    destroy_source_rgba_buffers(w);
    w->vsr_input_width = 0;
    w->vsr_input_height = 0;
    w->vsr_output_width = 0;
    w->vsr_output_height = 0;
}

static int ensure_source_rgba_buffers(BakeWorker* w, int input_w, int input_h) {
    const size_t bytes = (size_t)input_w * (size_t)input_h * 4;
    if (w->pre_vsr_rgba && w->prev_pre_vsr_rgba && w->interp_pre_vsr_rgba &&
        w->pre_vsr_rgba_bytes == bytes &&
        w->vsr_input_width == input_w && w->vsr_input_height == input_h) {
        return 0;
    }
    nvof_destroy(w);
    destroy_source_rgba_buffers(w);
    w->pre_vsr_rgba_bytes = bytes;
    CHECK_CU(cuMemAlloc(&w->pre_vsr_rgba, bytes), "allocPreRgba");
    CHECK_CU(cuMemAlloc(&w->prev_pre_vsr_rgba, bytes), "allocPrevPreRgba");
    CHECK_CU(cuMemAlloc(&w->interp_pre_vsr_rgba, bytes), "allocInterpPreRgba");
    w->vsr_input_width = input_w;
    w->vsr_input_height = input_h;
    return 0;
bail:
    destroy_source_rgba_buffers(w);
    w->vsr_input_width = 0;
    w->vsr_input_height = 0;
    return -1;
}

static int configure_maxine_effect_for_input(BakeWorker* w, int input_w, int input_h) {
    const int maxine_output_w = BAKE_OUTPUT_WIDTH;
    const int maxine_output_h = BAKE_OUTPUT_HEIGHT;
    const int maxine_effect_matches =
        w->vsr_effect && w->maxine_input_width == input_w && w->maxine_input_height == input_h &&
        w->vsr_output_width == maxine_output_w && w->vsr_output_height == maxine_output_h;
    if (input_w > BAKE_OUTPUT_WIDTH || input_h > BAKE_OUTPUT_HEIGHT) {
        BAIL_FMT("vsr_input_larger_than_output=%dx%d", input_w, input_h);
    }
    if (maxine_effect_matches) {
        bake_write_runtime_state(w, input_w, input_h, w->graph_model_input_h);
        return 0;
    }

    destroy_maxine_effect(w);
    CHECK_NV(NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &w->vsr_effect), "createEffect");
    CHECK_NV(NvVFX_SetU32(w->vsr_effect, NVVFX_QUALITY_LEVEL, 4), "setQuality");
    CHECK_NV(NvVFX_SetCudaStream(w->vsr_effect, NVVFX_CUDA_STREAM, w->cu_stream), "setStream");

    const float sx = (float)maxine_output_w / (float)input_w;
    const float sy = (float)maxine_output_h / (float)input_h;
    if (fabsf(sx - sy) < 0.01f) {
        NvCV_Status scale_status = NvVFX_SetF32(w->vsr_effect, NVVFX_SCALE, sx);
        if (scale_status != NVCV_SUCCESS &&
            scale_status != NVCV_ERR_SELECTOR &&
            scale_status != NVCV_ERR_PARAMETER &&
            scale_status != NVCV_ERR_UNIMPLEMENTED) {
            BAIL_FMT("vfx_setScale_err=%d scale=%.3f", scale_status, sx);
        }
        if (scale_status != NVCV_SUCCESS) {
            fprintf(stderr,
                    "[d_native_processor] VSR scale selector ignored status=%d scale=%.3f; using bound input/output image dimensions\n",
                    scale_status, sx);
        }
    }

    CUdeviceptr dst_ptr = w->vsr_out_rgba;
    CUdeviceptr src_ptr = w->d_pipeline_rgba_a ? w->d_pipeline_rgba_a : w->pre_vsr_rgba;
    if (wrap_rgba_nvcv(&w->vsr_src_im, (void*)(uintptr_t)src_ptr, input_w, input_h) < 0) goto bail;
    if (wrap_rgba_nvcv(&w->vsr_dst_im, (void*)(uintptr_t)dst_ptr, maxine_output_w, maxine_output_h) < 0) goto bail;
    CHECK_NV(NvVFX_SetImage(w->vsr_effect, NVVFX_INPUT_IMAGE, &w->vsr_src_im), "setInImg");
    CHECK_NV(NvVFX_SetImage(w->vsr_effect, NVVFX_OUTPUT_IMAGE, &w->vsr_dst_im), "setOutImg");

    fprintf(stderr,
            "[d_native_processor] Maxine VSR load start: %dx%d RGBA -> %dx%d RGBA\n",
            input_w, input_h, maxine_output_w, maxine_output_h);
    double t_vsr_load = now_seconds();
    NvCV_Status load_status = NvVFX_Load(w->vsr_effect);
    if (load_status != NVCV_SUCCESS) {
        BAIL_FMT("vfx_loadEffect_err=%d input=%dx%d output=%dx%d",
                 load_status, input_w, input_h, maxine_output_w, maxine_output_h);
    }
    w->maxine_input_width = input_w;
    w->maxine_input_height = input_h;
    w->vsr_output_width = maxine_output_w;
    w->vsr_output_height = maxine_output_h;
    if (w->vsr_output_width != BAKE_OUTPUT_WIDTH || w->vsr_output_height != BAKE_OUTPUT_HEIGHT) {
        BAIL_FMT("maxine_non_canon_output_forbidden=%dx%d canon=%dx%d",
                 w->vsr_output_width, w->vsr_output_height,
                 BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT);
    }
    bake_write_runtime_state(w, input_w, input_h, w->graph_model_input_h);
    fprintf(stderr, "[d_native_processor] Maxine VSR ULTRA loaded in %.2fs: %dx%d RGBA -> canon %dx%d RGBA; filters=post4k+fruc no_post_vsr_upscale=1\n",
            now_seconds() - t_vsr_load,
            input_w, input_h, BAKE_OUTPUT_WIDTH, BAKE_OUTPUT_HEIGHT);
    return 0;

bail:
    destroy_maxine_effect(w);
    return -1;
}

static int configure_vsr_for_input(BakeWorker* w, int input_w, int input_h) {
    if (!w || !w->worker_ready) BAIL("worker_not_ready");
    if (input_w <= 0 || input_h <= 0) BAIL_FMT("bad_vsr_input_dims=%dx%d", input_w, input_h);
    BAIL("d_pipeline_model_stages_missing");

bail:
    destroy_vsr_effect(w);
    return -1;
}
