int bake_run_prepared(BakeWorker* w, const BakeRequest* req, BakeResult* res) {
    memset(res, 0, sizeof(*res));
    g_last_error[0] = 0;
    if (!g_prepared_valid) { snprintf(res->error, sizeof(res->error), "not_prepared"); return -1; }
    double t0 = now_seconds();
    BakeCtx c = g_prepared_ctx;
    memset(&g_prepared_ctx, 0, sizeof(g_prepared_ctx));
    g_prepared_valid = 0;
    if (seek_input(&c, req->start_seconds) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "seek_input", sizeof(res->error)-1);
        res->elapsed_seconds = now_seconds() - t0;
        close_ctx(&c);
        return -1;
    }
    if (ensure_worker_ready(w) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "worker_init", sizeof(res->error)-1);
        res->elapsed_seconds = now_seconds() - t0;
        close_ctx(&c);
        return -1;
    }
    apply_request_runtime(w, req);
    if (parse_pipeline_manifests(&c, req) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "pipeline_manifest", sizeof(res->error)-1);
        res->elapsed_seconds = now_seconds() - t0;
        close_ctx(&c);
        return -1;
    }
    if (configure_vsr_for_ctx(w, &c) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "vsr_configure", sizeof(res->error)-1);
        res->elapsed_seconds = now_seconds() - t0;
        close_ctx(&c);
        return -1;
    }
    if (open_output(&c, w, req) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "open_output", sizeof(res->error)-1);
        res->elapsed_seconds = now_seconds() - t0;
        close_ctx(&c);
        return -1;
    }
    int rc = run_pipeline(&c, w, req, res, t0);
    close_ctx(&c);
    return rc;
}

int bake_run(BakeWorker* w, const BakeRequest* req, BakeResult* res) {
    BakeCtx c = {0};
    memset(res, 0, sizeof(*res));
    g_last_error[0] = 0;
    double t0 = now_seconds();

    if (av_hwdevice_ctx_create(&c.hw_device_ref, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0) < 0) {
        strncpy(res->error, "hwdev_create", sizeof(res->error)-1);
        return -1;
    }
    if (open_input(&c, req) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "open_input", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    if (ensure_worker_ready(w) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "worker_init", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    apply_request_runtime(w, req);
    if (parse_pipeline_manifests(&c, req) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "pipeline_manifest", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    if (configure_vsr_for_ctx(w, &c) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "vsr_configure", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    if (seek_input(&c, req->start_seconds) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "seek_input", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    if (open_output(&c, w, req) < 0) {
        strncpy(res->error, g_last_error[0] ? g_last_error : "open_output", sizeof(res->error)-1);
        close_ctx(&c); return -1;
    }
    int rc = run_pipeline(&c, w, req, res, t0);
    close_ctx(&c);
    return rc;
}
