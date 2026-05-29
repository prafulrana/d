typedef NV_OF_STATUS (NVOFAPI *PFNNVOFAPICREATEINSTANCECUDA_LOCAL)(uint32_t apiVer, NV_OF_CUDA_API_FUNCTION_LIST* functionList);

static uint32_t pick_nvof_row_pitch(const NV_OF_CUDA_BUFFER_STRIDE_INFO* stride, uint32_t min_pitch) {
    const uint32_t sx = stride->strideInfo[0].strideXInBytes;
    const uint32_t sy = stride->strideInfo[0].strideYInBytes;
    if (sx >= min_pitch) return sx;
    if (sy >= min_pitch) return sy;
    return min_pitch;
}

// nvof_init now takes the target resolution explicitly. Legacy path
// passes BAKE_OUTPUT_WIDTH/HEIGHT (4K); the source-NVOF data-plane path
// passes the decoded source dims (e.g. 1280×720). Flow-vector counts
// scale linearly with width × height, so at 720p we do ~9× less work
// per NvOFExecute() than at 4K.
int nvof_init(BakeWorker* w, int target_w, int target_h) {
    w->nvof_target_w = target_w;
    w->nvof_target_h = target_h;
    w->nvof_grid = 4;
    w->nvof_flow_w = (target_w + w->nvof_grid - 1) / w->nvof_grid;
    w->nvof_flow_h = (target_h + w->nvof_grid - 1) / w->nvof_grid;

    w->nvof_lib = dlopen("libnvidia-opticalflow.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!w->nvof_lib) w->nvof_lib = dlopen("libnvidia-opticalflow.so", RTLD_NOW | RTLD_GLOBAL);
    if (!w->nvof_lib) BAIL_FMT("dlopen_libnvidia_opticalflow_failed=%.200s", dlerror());

    PFNNVOFAPICREATEINSTANCECUDA_LOCAL create_api =
        (PFNNVOFAPICREATEINSTANCECUDA_LOCAL)dlsym(w->nvof_lib, "NvOFAPICreateInstanceCuda");
    if (!create_api) BAIL("nvof_create_instance_symbol_missing");
    memset(&w->nvof_api, 0, sizeof(w->nvof_api));
    CHECK_OF(create_api(NV_OF_API_VERSION, &w->nvof_api), "createInstanceCuda");
    if (!w->nvof_api.nvCreateOpticalFlowCuda || !w->nvof_api.nvOFInit ||
        !w->nvof_api.nvOFCreateGPUBufferCuda || !w->nvof_api.nvOFGPUBufferGetCUdeviceptr ||
        !w->nvof_api.nvOFGPUBufferGetStrideInfo || !w->nvof_api.nvOFExecute) {
        BAIL("nvof_incomplete_function_table");
    }

    CHECK_OF(w->nvof_api.nvCreateOpticalFlowCuda(w->cu_ctx, &w->nvof_handle), "createOpticalFlowCuda");
    if (w->nvof_api.nvOFSetIOCudaStreams) {
        CHECK_OF(w->nvof_api.nvOFSetIOCudaStreams(w->nvof_handle, w->cu_stream, w->cu_stream), "setStreams");
    }

    NV_OF_INIT_PARAMS init;
    memset(&init, 0, sizeof(init));
    init.width = (uint32_t)target_w;
    init.height = (uint32_t)target_h;
    init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    CHECK_OF(w->nvof_api.nvOFInit(w->nvof_handle, &init), "init");

    NV_OF_BUFFER_DESCRIPTOR in_desc;
    memset(&in_desc, 0, sizeof(in_desc));
    in_desc.width = (uint32_t)target_w;
    in_desc.height = (uint32_t)target_h;
    in_desc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
    in_desc.bufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
    CHECK_OF(w->nvof_api.nvOFCreateGPUBufferCuda(w->nvof_handle, &in_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &w->nvof_prev_buffer), "createPrevBuffer");
    CHECK_OF(w->nvof_api.nvOFCreateGPUBufferCuda(w->nvof_handle, &in_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &w->nvof_curr_buffer), "createCurrBuffer");

    NV_OF_BUFFER_DESCRIPTOR out_desc;
    memset(&out_desc, 0, sizeof(out_desc));
    out_desc.width = (uint32_t)w->nvof_flow_w;
    out_desc.height = (uint32_t)w->nvof_flow_h;
    out_desc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
    out_desc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;
    CHECK_OF(w->nvof_api.nvOFCreateGPUBufferCuda(w->nvof_handle, &out_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &w->nvof_flow_buffer), "createFlowBuffer");
    CHECK_OF(w->nvof_api.nvOFCreateGPUBufferCuda(w->nvof_handle, &out_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &w->nvof_reverse_flow_buffer), "createReverseFlowBuffer");

    w->nvof_prev_ptr = w->nvof_api.nvOFGPUBufferGetCUdeviceptr(w->nvof_prev_buffer);
    w->nvof_curr_ptr = w->nvof_api.nvOFGPUBufferGetCUdeviceptr(w->nvof_curr_buffer);
    w->nvof_flow_ptr = w->nvof_api.nvOFGPUBufferGetCUdeviceptr(w->nvof_flow_buffer);
    w->nvof_reverse_flow_ptr = w->nvof_api.nvOFGPUBufferGetCUdeviceptr(w->nvof_reverse_flow_buffer);
    if (!w->nvof_prev_ptr || !w->nvof_curr_ptr || !w->nvof_flow_ptr || !w->nvof_reverse_flow_ptr) BAIL("nvof_deviceptr_missing");

    NV_OF_CUDA_BUFFER_STRIDE_INFO stride;
    memset(&stride, 0, sizeof(stride));
    CHECK_OF(w->nvof_api.nvOFGPUBufferGetStrideInfo(w->nvof_prev_buffer, &stride), "prevStride");
    const uint32_t prev_sx = stride.strideInfo[0].strideXInBytes;
    const uint32_t prev_sy = stride.strideInfo[0].strideYInBytes;
    w->nvof_prev_pitch = pick_nvof_row_pitch(&stride, (uint32_t)target_w * 4);
    memset(&stride, 0, sizeof(stride));
    CHECK_OF(w->nvof_api.nvOFGPUBufferGetStrideInfo(w->nvof_curr_buffer, &stride), "currStride");
    const uint32_t curr_sx = stride.strideInfo[0].strideXInBytes;
    const uint32_t curr_sy = stride.strideInfo[0].strideYInBytes;
    w->nvof_curr_pitch = pick_nvof_row_pitch(&stride, (uint32_t)target_w * 4);
    memset(&stride, 0, sizeof(stride));
    CHECK_OF(w->nvof_api.nvOFGPUBufferGetStrideInfo(w->nvof_flow_buffer, &stride), "flowStride");
    const uint32_t flow_sx = stride.strideInfo[0].strideXInBytes;
    const uint32_t flow_sy = stride.strideInfo[0].strideYInBytes;
    w->nvof_flow_pitch = pick_nvof_row_pitch(&stride, (uint32_t)w->nvof_flow_w * sizeof(int16_t) * 2);
    memset(&stride, 0, sizeof(stride));
    CHECK_OF(w->nvof_api.nvOFGPUBufferGetStrideInfo(w->nvof_reverse_flow_buffer, &stride), "reverseFlowStride");
    const uint32_t rev_flow_sx = stride.strideInfo[0].strideXInBytes;
    const uint32_t rev_flow_sy = stride.strideInfo[0].strideYInBytes;
    w->nvof_reverse_flow_pitch = pick_nvof_row_pitch(&stride, (uint32_t)w->nvof_flow_w * sizeof(int16_t) * 2);

    if (!w->nvof_prev_pitch) w->nvof_prev_pitch = (uint32_t)target_w * 4;
    if (!w->nvof_curr_pitch) w->nvof_curr_pitch = (uint32_t)target_w * 4;
    if (!w->nvof_flow_pitch) w->nvof_flow_pitch = (uint32_t)w->nvof_flow_w * sizeof(int16_t) * 2;
    if (!w->nvof_reverse_flow_pitch) w->nvof_reverse_flow_pitch = (uint32_t)w->nvof_flow_w * sizeof(int16_t) * 2;

    fprintf(stderr, "[d_native_processor] NVIDIA Optical Flow FRUC ready: %dx%d grid=%d flow=%dx%d input_pitch=%u/%u flow_pitch=%u/%u raw_stride prev=%ux%u curr=%ux%u flow=%ux%u rev=%ux%u bidirectional=1 temporal_hints=0\n",
            target_w, target_h, w->nvof_grid,
            w->nvof_flow_w, w->nvof_flow_h,
            w->nvof_prev_pitch, w->nvof_curr_pitch,
            w->nvof_flow_pitch, w->nvof_reverse_flow_pitch,
            prev_sx, prev_sy, curr_sx, curr_sy, flow_sx, flow_sy, rev_flow_sx, rev_flow_sy);
    return 0;
bail:
    return -1;
}

void nvof_destroy(BakeWorker* w) {
    if (!w) return;
    if (w->nvof_api.nvOFDestroyGPUBufferCuda) {
        if (w->nvof_reverse_flow_buffer) { w->nvof_api.nvOFDestroyGPUBufferCuda(w->nvof_reverse_flow_buffer); w->nvof_reverse_flow_buffer = NULL; }
        if (w->nvof_flow_buffer) { w->nvof_api.nvOFDestroyGPUBufferCuda(w->nvof_flow_buffer); w->nvof_flow_buffer = NULL; }
        if (w->nvof_curr_buffer) { w->nvof_api.nvOFDestroyGPUBufferCuda(w->nvof_curr_buffer); w->nvof_curr_buffer = NULL; }
        if (w->nvof_prev_buffer) { w->nvof_api.nvOFDestroyGPUBufferCuda(w->nvof_prev_buffer); w->nvof_prev_buffer = NULL; }
    }
    if (w->nvof_api.nvOFDestroy && w->nvof_handle) {
        w->nvof_api.nvOFDestroy(w->nvof_handle);
        w->nvof_handle = NULL;
    }
    if (w->nvof_lib) {
        dlclose(w->nvof_lib);
        w->nvof_lib = NULL;
    }
    w->nvof_ready = 0;
    w->nvof_target_w = 0;
    w->nvof_target_h = 0;
}

static int copy_rgba_to_nvof_input(BakeWorker* w, CUdeviceptr src, CUdeviceptr dst, uint32_t dst_pitch) {
    launch_rgba_to_abgr_pitch(
        (void*)(uintptr_t)src,
        (void*)(uintptr_t)dst,
        w->nvof_target_h, w->nvof_target_w,
        (int)dst_pitch,
        (void*)w->cu_stream
    );
    return 0;
}

int nvof_execute(BakeWorker* w, CUdeviceptr prev_rgba, CUdeviceptr curr_rgba) {
    if (!w->nvof_handle) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_not_initialized");
        return -1;
    }
    if (copy_rgba_to_nvof_input(w, prev_rgba, w->nvof_prev_ptr, w->nvof_prev_pitch) < 0) return -1;
    if (copy_rgba_to_nvof_input(w, curr_rgba, w->nvof_curr_ptr, w->nvof_curr_pitch) < 0) return -1;

    NV_OF_EXECUTE_INPUT_PARAMS in;
    memset(&in, 0, sizeof(in));
    in.inputFrame = w->nvof_prev_buffer;
    in.referenceFrame = w->nvof_curr_buffer;
    in.disableTemporalHints = NV_OF_TRUE;
    NV_OF_EXECUTE_OUTPUT_PARAMS out;
    memset(&out, 0, sizeof(out));
    out.outputBuffer = w->nvof_flow_buffer;
    NV_OF_STATUS s = w->nvof_api.nvOFExecute(w->nvof_handle, &in, &out);
    if (s != NV_OF_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_execute_fwd_err=%d", s);
        return -1;
    }

    memset(&in, 0, sizeof(in));
    in.inputFrame = w->nvof_curr_buffer;
    in.referenceFrame = w->nvof_prev_buffer;
    in.disableTemporalHints = NV_OF_TRUE;
    memset(&out, 0, sizeof(out));
    out.outputBuffer = w->nvof_reverse_flow_buffer;
    s = w->nvof_api.nvOFExecute(w->nvof_handle, &in, &out);
    if (s != NV_OF_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_execute_rev_err=%d", s);
        return -1;
    }
    CUresult cs = cuEventRecord(w->ev_nvof_done, w->cu_stream);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_event_record=%d", cs);
        return -1;
    }
    cs = cuStreamWaitEvent(w->cu_stream_b, w->ev_nvof_done, 0);
    if (cs != CUDA_SUCCESS) {
        snprintf(g_last_error, sizeof(g_last_error), "nvof_event_wait=%d", cs);
        return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Worker init / destroy
// ----------------------------------------------------------------------------

