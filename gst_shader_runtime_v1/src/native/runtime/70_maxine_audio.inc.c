static int open_audio_decoder(BakeCtx* c) {
    if (c->audio_stream_idx < 0) BAIL("no_audio_stream");
    AVStream* st = c->in_fmt->streams[c->audio_stream_idx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        BAIL_FMT("no_audio_decoder codec=%s", avcodec_get_name(st->codecpar->codec_id));
    }
    c->audio_dec_ctx = avcodec_alloc_context3(dec);
    if (!c->audio_dec_ctx) BAIL("audio_dec_alloc");
    if (avcodec_parameters_to_context(c->audio_dec_ctx, st->codecpar) < 0) BAIL("audio_dec_params");
    c->audio_dec_ctx->pkt_timebase = st->time_base;
    if (c->audio_dec_ctx->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&c->audio_dec_ctx->ch_layout, 2);
    }
    if (avcodec_open2(c->audio_dec_ctx, dec, NULL) < 0) {
        BAIL_FMT("audio_decoder_open codec=%s", avcodec_get_name(st->codecpar->codec_id));
    } else {
        fprintf(stderr,
                "[d_native_processor] audio decoder open codec=%s decoder=%s ch=%d sr=%d fmt=%s time_base=%d/%d bit_rate=%lld\n",
                avcodec_get_name(st->codecpar->codec_id),
                dec->name ? dec->name : "",
                c->audio_dec_ctx->ch_layout.nb_channels,
                c->audio_dec_ctx->sample_rate,
                av_get_sample_fmt_name(c->audio_dec_ctx->sample_fmt),
                st->time_base.num, st->time_base.den,
                (long long)st->codecpar->bit_rate);
    }
    return 0;
bail:
    return -1;
}

static const char* afx_status_name(NvAFX_Status status) {
    switch (status) {
        case NVAFX_STATUS_SUCCESS: return "success";
        case NVAFX_STATUS_FAILED: return "failed";
        case NVAFX_STATUS_INVALID_HANDLE: return "invalid_handle";
        case NVAFX_STATUS_INVALID_PARAM: return "invalid_param";
        case NVAFX_STATUS_IMMUTABLE_PARAM: return "immutable_param";
        case NVAFX_STATUS_INSUFFICIENT_DATA: return "insufficient_data";
        case NVAFX_STATUS_EFFECT_NOT_AVAILABLE: return "effect_not_available";
        case NVAFX_STATUS_OUTPUT_BUFFER_TOO_SMALL: return "output_buffer_too_small";
        case NVAFX_STATUS_MODEL_LOAD_FAILED: return "model_load_failed";
        case NVAFX_STATUS_MODEL_NOT_LOADED: return "model_not_loaded";
        case NVAFX_STATUS_INCOMPATIBLE_MODEL: return "incompatible_model";
        case NVAFX_STATUS_GPU_UNSUPPORTED: return "gpu_unsupported";
        case NVAFX_STATUS_NO_SUPPORTED_GPU_FOUND: return "no_supported_gpu";
        case NVAFX_STATUS_WRONG_GPU: return "wrong_gpu";
        case NVAFX_STATUS_CUDA_ERROR: return "cuda_error";
        case NVAFX_STATUS_INVALID_OPERATION: return "invalid_operation";
        case NVAFX_UNSUPPORTED_RUNTIME: return "unsupported_runtime";
        default: return "unknown";
    }
}

static int set_maxine_audio_cleanup_strength(BakeCtx* c, float cleanup_strength, int fatal) {
    if (!c || !c->afx.handle) return 0;
    cleanup_strength = clamp_float(cleanup_strength, 0.0f, 1.0f);
    float intensity[2] = { cleanup_strength, 1.0f };
    NvAFX_Status status = NvAFX_SetFloatList(
        c->afx.handle, NVAFX_PARAM_INTENSITY_RATIO, intensity, 2);
    if (status != NVAFX_STATUS_SUCCESS && status != NVAFX_STATUS_INVALID_PARAM) {
        if (fatal) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "maxine_audio_intensity=%s", afx_status_name(status));
            return -1;
        }
        fprintf(stderr,
                "[d_native_processor] live audio cleanup update ignored status=%s strength=%.3f\n",
                afx_status_name(status), cleanup_strength);
        return 0;
    }
    c->audio_cleanup_strength = cleanup_strength;
    return 0;
}

static int path_exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int join_path(char* out, size_t out_size, const char* a, const char* b) {
    if (snprintf(out, out_size, "%s/%s", a, b) >= (int)out_size) {
        snprintf(g_last_error, sizeof(g_last_error), "path_too_long");
        return -1;
    }
    return 0;
}

static int find_model_file(
    char* out, size_t out_size,
    const char* root, const char* rel_dir,
    const char* exact_name, const char* prefix)
{
    char dir[PATH_MAX];
    if (join_path(dir, sizeof(dir), root, rel_dir) < 0) return -1;

    char exact_path[PATH_MAX];
    if (join_path(exact_path, sizeof(exact_path), dir, exact_name) < 0) return -1;
    if (path_exists(exact_path)) {
        snprintf(out, out_size, "%s", exact_path);
        fprintf(stderr, "[d_native_processor] Maxine Audio model exact: %s\n", out);
        return 0;
    }

    DIR* d = opendir(dir);
    if (!d) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_model_dir_missing=%s", dir);
        return -1;
    }

    const size_t prefix_len = strlen(prefix);
    int found = 0;
    struct dirent* ent = NULL;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (!strstr(name, ".trtpkg")) continue;
        if (join_path(out, out_size, dir, name) < 0) {
            closedir(d);
            return -1;
        }
        found = 1;
        break;
    }
    closedir(d);
    if (!found) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "maxine_audio_model_missing dir=%s prefix=%s", dir, prefix);
        return -1;
    }
    fprintf(stderr, "[d_native_processor] Maxine Audio model discovered: %s\n", out);
    return 0;
}

static int preload_maxine_audio_libs(const char* root) {
    const char* rels[] = {
        "nvafx/lib/libnv_audiofx.so.2",
        "features/dereverb_denoiser/lib/libnv_audiofx_dereverb_denoiser.so.2",
        "features/superres/lib/libnv_audiofx_superres.so.2",
        NULL,
    };
    for (int i = 0; rels[i]; i++) {
        char path[PATH_MAX];
        if (join_path(path, sizeof(path), root, rels[i]) < 0) return -1;
        void* lib = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!lib) {
            snprintf(g_last_error, sizeof(g_last_error),
                     "maxine_audio_dlopen_failed=%s err=%.160s", path, dlerror());
            return -1;
        }
        fprintf(stderr, "[d_native_processor] Maxine Audio dlopen ok: %s\n", path);
    }
    return 0;
}

static int init_maxine_audio_chain(BakeCtx* c) {
    if (!c->audio_dec_ctx) return 0;
    const char* root = getenv("DPROC_MAXINE_AUDIO_ROOT");
    if (!root || !*root) root = DPROC_AFX_DEFAULT_ROOT;
    const char* sm = getenv("DPROC_MAXINE_AUDIO_SM");
    if (!sm || !*sm) sm = DPROC_AFX_SM_DEFAULT;
    fprintf(stderr, "[d_native_processor] Maxine Audio init start root=%s sm=%s\n", root, sm);

    if (preload_maxine_audio_libs(root) < 0) return -1;

    char rel_dir[PATH_MAX];
    snprintf(rel_dir, sizeof(rel_dir), "features/dereverb_denoiser/models/%s", sm);
    if (find_model_file(c->afx.model_dereverb_denoiser, sizeof(c->afx.model_dereverb_denoiser),
                        root, rel_dir,
                        "dereverb_denoiser_16k.trtpkg",
                        "dereverb_denoiser_16k") < 0) return -1;
    snprintf(rel_dir, sizeof(rel_dir), "features/superres/models/%s", sm);
    if (find_model_file(c->afx.model_superres, sizeof(c->afx.model_superres),
                        root, rel_dir,
                        "superres_16k_to_48k.trtpkg",
                        "superres_16k_to_48k") < 0) return -1;

    NvAFX_Status status = NvAFX_CreateChainedEffect(
        NVAFX_CHAINED_EFFECT_DEREVERB_DENOISER_16k_SUPERRES_16k_TO_48k,
        &c->afx.handle);
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_create=%s", afx_status_name(status));
        return -1;
    }

    (void)NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_USE_DEFAULT_GPU, 1);
    (void)NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_ENABLE_VAD, 0);
    (void)NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_INPUT_SAMPLE_RATE, DPROC_AFX_INPUT_RATE);
    (void)NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE, DPROC_AFX_OUTPUT_RATE);
    status = NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_NUM_STREAMS, 1);
    if (status != NVAFX_STATUS_SUCCESS && status != NVAFX_STATUS_INVALID_PARAM) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_num_streams=%s", afx_status_name(status));
        goto bail;
    }

    const char* models[2] = { c->afx.model_dereverb_denoiser, c->afx.model_superres };
    status = NvAFX_SetStringList(c->afx.handle, NVAFX_PARAM_MODEL_PATH, models, 2);
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_models=%s", afx_status_name(status));
        goto bail;
    }

    unsigned int list_size = 0;
    status = NvAFX_GetU32List(c->afx.handle, NVAFX_PARAM_SUPPORTED_NUM_SAMPLES_PER_FRAME,
                              NULL, &list_size);
    if (status != NVAFX_STATUS_OUTPUT_BUFFER_TOO_SMALL || list_size == 0) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_supported_frames=%s size=%u",
                 afx_status_name(status), list_size);
        goto bail;
    }
    unsigned int* supported = av_malloc_array(list_size, sizeof(unsigned int));
    if (!supported) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_supported_alloc");
        goto bail;
    }
    status = NvAFX_GetU32List(c->afx.handle, NVAFX_PARAM_SUPPORTED_NUM_SAMPLES_PER_FRAME,
                              supported, &list_size);
    if (status != NVAFX_STATUS_SUCCESS) {
        av_free(supported);
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_supported_read=%s", afx_status_name(status));
        goto bail;
    }
    const unsigned int preferred_samples =
        (unsigned int)env_int_clamped("DPROC_MAXINE_AUDIO_FRAME_SAMPLES", 1600, 160, 4800);
    c->afx.input_samples = supported[0];
    for (unsigned int i = 0; i < list_size; i++) {
        if (supported[i] <= preferred_samples && supported[i] >= c->afx.input_samples) {
            c->afx.input_samples = supported[i];
        }
    }
    av_free(supported);

    status = NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_NUM_SAMPLES_PER_INPUT_FRAME,
                          c->afx.input_samples);
    if (status == NVAFX_STATUS_INVALID_PARAM) {
        status = NvAFX_SetU32(c->afx.handle, NVAFX_PARAM_NUM_SAMPLES_PER_FRAME,
                              c->afx.input_samples);
    }
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_frame_size=%s", afx_status_name(status));
        goto bail;
    }

    status = NvAFX_Load(c->afx.handle);
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_load=%s", afx_status_name(status));
        goto bail;
    }

    if (set_maxine_audio_cleanup_strength(c, c->audio_cleanup_strength, 1) < 0) {
        goto bail;
    }

    status = NvAFX_GetU32(c->afx.handle, NVAFX_PARAM_NUM_SAMPLES_PER_OUTPUT_FRAME,
                          &c->afx.output_samples);
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_output_frame=%s", afx_status_name(status));
        goto bail;
    }
    status = NvAFX_GetU32(c->afx.handle, NVAFX_PARAM_OUTPUT_SAMPLE_RATE,
                          &c->afx.output_sample_rate);
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_output_rate=%s", afx_status_name(status));
        goto bail;
    }
    status = NvAFX_GetU32(c->afx.handle, NVAFX_PARAM_NUM_INPUT_CHANNELS,
                          &c->afx.input_channels);
    if (status != NVAFX_STATUS_SUCCESS) {
        status = NvAFX_GetU32(c->afx.handle, NVAFX_PARAM_NUM_CHANNELS,
                              &c->afx.input_channels);
    }
    if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_input_channels=%s", afx_status_name(status));
        goto bail;
    }
    status = NvAFX_GetU32(c->afx.handle, NVAFX_PARAM_NUM_OUTPUT_CHANNELS,
                          &c->afx.output_channels);
    if (status == NVAFX_STATUS_INVALID_PARAM) c->afx.output_channels = c->afx.input_channels;
    else if (status != NVAFX_STATUS_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_output_channels=%s", afx_status_name(status));
        goto bail;
    }
    if (c->afx.input_channels != DPROC_AFX_INPUT_CHANNELS ||
        c->afx.output_sample_rate != DPROC_AFX_OUTPUT_RATE ||
        c->afx.output_channels < 1 || c->afx.output_channels > 2) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "maxine_audio_contract in_ch=%u out_ch=%u out_rate=%u",
                 c->afx.input_channels, c->afx.output_channels, c->afx.output_sample_rate);
        goto bail;
    }

    c->afx.input_buffer_samples = c->afx.input_samples;
    c->afx.output_buffer_samples = (size_t)c->afx.output_samples * c->afx.output_channels;
    c->afx.input_buffer = av_mallocz(c->afx.input_buffer_samples * sizeof(float));
    c->afx.output_buffer = av_mallocz(c->afx.output_buffer_samples * sizeof(float));
    if (!c->afx.input_buffer || !c->afx.output_buffer) {
        snprintf(g_last_error, sizeof(g_last_error), "maxine_audio_reusable_buffers_alloc");
        goto bail;
    }

    fprintf(stderr,
            "[d_native_processor] Maxine Audio ready: mode=%s intensity=%.2f + AudioSuperRes16to48 frame=%u->%u out_ch=%u models=%s,%s\n",
            c->audio_cleanup_strength <= 0.0001f ? "audiosr_no_cleanup_chain" : "entertainment_light_cleanup16k",
            c->audio_cleanup_strength, c->afx.input_samples, c->afx.output_samples, c->afx.output_channels,
            c->afx.model_dereverb_denoiser,
            c->afx.model_superres);
    return 0;

bail:
    destroy_maxine_audio_chain(&c->afx);
    return -1;
}

static void destroy_maxine_audio_chain(MaxineAudioChain* chain) {
    if (!chain) return;
    if (chain->handle) {
        NvAFX_DestroyEffect(chain->handle);
        chain->handle = NULL;
    }
    if (chain->input_buffer) {
        av_free(chain->input_buffer);
        chain->input_buffer = NULL;
    }
    if (chain->output_buffer) {
        av_free(chain->output_buffer);
        chain->output_buffer = NULL;
    }
    chain->input_buffer_samples = 0;
    chain->output_buffer_samples = 0;
    chain->input_samples = 0;
    chain->output_samples = 0;
    chain->input_channels = 0;
    chain->output_channels = 0;
    chain->output_sample_rate = 0;
}

// Force libavformat's MKV demuxer to load + cache the cluster index now.
// We seek to ~30s into the file (well past the header / first keyframe
// but small enough to be cheap) and then rewind back to 0. The index
// fetch is the slow operation on remote MKVs; once cached, subsequent
// av_seek_frame calls hit memory and complete in ~1 s.
