static void maybe_open_perf_ring(BakeCtx* c, const BakeRequest* req) {
    if (!req->perf_ring_path || !req->perf_ring_path[0] || c->perf_ring_active) return;
    if (perf_ring_open_writer(&c->perf_ring, req->perf_ring_path) == 0) {
        c->perf_ring_active = 1;
        fprintf(stderr, "[d_native_processor] perf ring opened path=%s slot_bytes=%u slots=%u\n",
                req->perf_ring_path, PERF_RING_SLOT_BYTES, PERF_RING_SLOT_COUNT);
    } else {
        fprintf(stderr, "[d_native_processor] perf ring open failed path=%s errno=%d\n",
                req->perf_ring_path, errno);
    }
}
