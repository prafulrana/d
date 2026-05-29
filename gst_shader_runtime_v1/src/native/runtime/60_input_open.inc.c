static int looks_like_live_input(const char* url) {
    if (!url || !*url) return 0;
    return strcasestr(url, ".m3u") != NULL
        || strcasestr(url, ".m3u8") != NULL
        || strcasestr(url, ".mpd") != NULL
        || strcasestr(url, ".ism") != NULL
        || strcasestr(url, ".isml") != NULL
        || strcasestr(url, ".f4m") != NULL
        || strcasestr(url, ".ts") != NULL
        || strcasestr(url, ".m2ts") != NULL
        || strcasestr(url, ".m2t") != NULL
        || strcasestr(url, ".mts") != NULL
        || strcasestr(url, ".m4s") != NULL
        || strcasestr(url, ".cmfv") != NULL
        || strcasestr(url, ".cmfa") != NULL
        || strcasestr(url, ".flv") != NULL
        || strcasestr(url, "/play/live.m3u8") != NULL
        || strcasestr(url, "/play/live.mpd") != NULL
        // NOTE: /play/fetch is used by BOTH live (HLS chunked .ts) and VOD
        // (the local-middleware mp4 proxy). Matching on it alone forces the
        // worker into live mode (seekable=0, no reconnect, discardcorrupt)
        // which kills VOD seek. The wrapping URL doesn't tell us live vs
        // VOD — trust the explicit `is_live` field from the spawn JSON
        // instead. Live HLS streams still match via /play/live.m3u8 above.
        || strcasestr(url, "/master.m3u8") != NULL
        || strcasestr(url, "/index.m3u8") != NULL
        || strcasestr(url, "/manifest") != NULL
        || strcasestr(url, "mpegurl") != NULL
        || strncasecmp(url, "rtmp://", 7) == 0
        || strncasecmp(url, "rtmpe://", 8) == 0
        || strncasecmp(url, "rtmpte://", 9) == 0
        || strncasecmp(url, "rtmps://", 8) == 0
        || strncasecmp(url, "rtmpt://", 8) == 0
        || strncasecmp(url, "rtmpts://", 9) == 0
        || strncasecmp(url, "ffrtmphttp://", 13) == 0
        || strncasecmp(url, "rtp://", 6) == 0
        || strncasecmp(url, "rtsp://", 7) == 0
        || strncasecmp(url, "rtsps://", 8) == 0
        || strncasecmp(url, "srt://", 6) == 0
        || strncasecmp(url, "sap://", 6) == 0
        || strncasecmp(url, "udp://", 6) == 0
        || strncasecmp(url, "tcp://", 6) == 0
        || strncasecmp(url, "mms://", 6) == 0
        || strncasecmp(url, "mmst://", 7) == 0
        || strncasecmp(url, "mmsh://", 7) == 0
        || strncasecmp(url, "rist://", 7) == 0;
}

static int looks_like_finite_playlist_input(const char* url) {
    if (!url || !*url) return 0;
    return strcasestr(url, ".m3u") != NULL
        || strcasestr(url, ".m3u8") != NULL
        || strcasestr(url, ".mpd") != NULL
        || strcasestr(url, ".ism") != NULL
        || strcasestr(url, ".isml") != NULL
        || strcasestr(url, ".f4m") != NULL
        || strcasestr(url, "/play/live.m3u8") != NULL
        || strcasestr(url, "/play/live.mpd") != NULL
        || strcasestr(url, "/master.m3u8") != NULL
        || strcasestr(url, "/index.m3u8") != NULL
        || strcasestr(url, "/manifest") != NULL
        || strcasestr(url, "mpegurl") != NULL;
}

static const char* cuvid_decoder_name(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:        return "h264_cuvid";
        case AV_CODEC_ID_HEVC:        return "hevc_cuvid";
        case AV_CODEC_ID_MPEG1VIDEO:  return "mpeg1_cuvid";
        case AV_CODEC_ID_MPEG2VIDEO:  return "mpeg2_cuvid";
        case AV_CODEC_ID_MPEG4:       return "mpeg4_cuvid";
        case AV_CODEC_ID_VC1:         return "vc1_cuvid";
        case AV_CODEC_ID_VP8:         return "vp8_cuvid";
        case AV_CODEC_ID_VP9:         return "vp9_cuvid";
        case AV_CODEC_ID_AV1:         return "av1_cuvid";
        case AV_CODEC_ID_MJPEG:       return "mjpeg_cuvid";
        default:                      return NULL;
    }
}

static const char* media_type_name(enum AVMediaType type) {
    switch (type) {
        case AVMEDIA_TYPE_VIDEO: return "video";
        case AVMEDIA_TYPE_AUDIO: return "audio";
        case AVMEDIA_TYPE_SUBTITLE: return "subtitle";
        case AVMEDIA_TYPE_DATA: return "data";
        default: return "other";
    }
}

static enum AVPixelFormat cuda_frame_sw_format(const AVFrame* frame) {
    if (!frame || !frame->hw_frames_ctx) return AV_PIX_FMT_NV12;
    const AVHWFramesContext* fctx = (const AVHWFramesContext*)frame->hw_frames_ctx->data;
    return fctx ? fctx->sw_format : AV_PIX_FMT_NV12;
}

static int yuv420_input_format_for_cuda_frame(const AVFrame* frame) {
    enum AVPixelFormat sw = cuda_frame_sw_format(frame);
    switch (sw) {
        case AV_PIX_FMT_NONE:
        case AV_PIX_FMT_NV12:
            return DPROC_YUV420_NV12;
        case AV_PIX_FMT_P010:
            return DPROC_YUV420_P010;
        case AV_PIX_FMT_P016:
            return DPROC_YUV420_P016;
        default:
            return -1;
    }
}

static void log_input_streams(AVFormatContext* fmt) {
    if (!fmt) return;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream* st = fmt->streams[i];
        AVCodecParameters* p = st ? st->codecpar : NULL;
        if (!p) continue;
        fprintf(stderr,
                "[d_native_processor] input stream #%u type=%s codec=%s w=%d h=%d ch=%d sr=%d time_base=%d/%d avg_fps=%d/%d r_fps=%d/%d bit_rate=%lld disposition=0x%x\n",
                i,
                media_type_name(p->codec_type),
                avcodec_get_name(p->codec_id),
                p->width,
                p->height,
                p->ch_layout.nb_channels,
                p->sample_rate,
                st->time_base.num, st->time_base.den,
                st->avg_frame_rate.num, st->avg_frame_rate.den,
                st->r_frame_rate.num, st->r_frame_rate.den,
                (long long)p->bit_rate,
                st->disposition);
    }
}

static int select_video_stream(AVFormatContext* fmt) {
    int fallback = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int best = -1;
    int64_t best_area = -1;
    int64_t best_bitrate = -1;
    for (unsigned i = 0; fmt && i < fmt->nb_streams; i++) {
        AVStream* st = fmt->streams[i];
        AVCodecParameters* p = st ? st->codecpar : NULL;
        if (!p || p->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (!avcodec_find_decoder(p->codec_id) && !cuvid_decoder_name(p->codec_id)) continue;
        int64_t area = (int64_t)p->width * (int64_t)p->height;
        int64_t bitrate = p->bit_rate;
        if (area <= 0 && i == (unsigned)fallback) area = 1;
        if (area > best_area || (area == best_area && bitrate > best_bitrate)) {
            best = (int)i;
            best_area = area;
            best_bitrate = bitrate;
        }
    }
    if (best >= 0) {
        AVCodecParameters* p = fmt->streams[best]->codecpar;
        fprintf(stderr,
                "[d_native_processor] video stream selected highest_rendition=%d w=%d h=%d bit_rate=%lld fallback=%d\n",
                best, p->width, p->height, (long long)p->bit_rate, fallback);
        return best;
    }
    return fallback;
}

static int select_audio_stream(AVFormatContext* fmt, int video_stream_idx) {
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, video_stream_idx, NULL, 0);
    if (idx >= 0) return idx;
    idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (idx >= 0) return idx;
    for (unsigned i = 0; fmt && i < fmt->nb_streams; i++) {
        AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(p->codec_id)) return (int)i;
    }
    return -1;
}

static int open_input(BakeCtx* c, const BakeRequest* req) {
    AVDictionary* opts = NULL;
    const int live_input = req->is_live || looks_like_live_input(req->url);
    const int finite_playlist_input = looks_like_finite_playlist_input(req->url);
    const int reconnect_at_eof = finite_playlist_input ? 0 : 1;
    fprintf(stderr,
            "[d_native_processor] open_input start url_present=%d live_input=%d local_play=%d proxy_present=%d ua_present=%d headers_present=%d start=%.3f duration=%.3f fps=%d bitrate=%d max=%d\n",
            req->url && *req->url ? 1 : 0,
            live_input,
            req->url && strstr(req->url, "/play/") ? 1 : 0,
            req->http_proxy && *req->http_proxy ? 1 : 0,
            req->user_agent && *req->user_agent ? 1 : 0,
            req->headers && *req->headers ? 1 : 0,
            req->start_seconds, req->duration_seconds, req->fps,
            req->bitrate_bps, req->max_bitrate_bps);
    if (req->user_agent && *req->user_agent && strcmp(req->user_agent, KODI_ANDROID_UA) != 0) {
        BAIL("kodi_android_user_agent_required");
    }
    if (req->headers && *req->headers && strcasestr(req->headers, "user-agent:")) {
        BAIL("kodi_android_user_agent_must_use_canonical_option");
    }
    if (req->http_proxy && *req->http_proxy) av_dict_set(&opts, "http_proxy", req->http_proxy, 0);
    av_dict_set(&opts, "user_agent", KODI_ANDROID_UA, 0);
    av_dict_set(&opts, "headers", (req->headers && *req->headers) ? req->headers : KODI_ANDROID_HEADERS, 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_at_eof", reconnect_at_eof ? "1" : "0", 0);
    av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(&opts, "reconnect_on_http_error", "4xx,5xx", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    // Hard ceiling on any single TCP read: 30s. Without this, libavformat
    // silently hangs forever when the source stops sending — workers never
    // return and the orchestrator's request timeout has to kill them. With
    // rw_timeout the read fails fast so the bake returns an error and the
    // pool can recycle the worker AND fall over to the other VPN proxy.
    // Microseconds.
    av_dict_set(&opts, "rw_timeout", "30000000", 0);
    av_dict_set(&opts, "timeout", "30000000", 0);
    // Movie files need a small fast-open probe. Live HLS is different: audio
    // can appear after the first media reload, in a sibling rendition, or in
    // the TS PMT after enough packets have arrived. Use a wider probe there
    // so we do not publish video-only output just because stream discovery was
    // too aggressive.
    char probe_buf[32];
    char analyze_buf[32];
    snprintf(probe_buf, sizeof(probe_buf), "%d",
             live_input
             ? env_int_clamped("DPROC_LIVE_PROBESIZE", 5000000, 262144, 20000000)
             : env_int_clamped("DPROC_FILE_PROBESIZE", 262144, 65536, 5000000));
    snprintf(analyze_buf, sizeof(analyze_buf), "%d",
             live_input
             ? env_int_clamped("DPROC_LIVE_ANALYZE_US", 5000000, 1000000, 20000000)
             : env_int_clamped("DPROC_FILE_ANALYZE_US", 1000000, 250000, 5000000));
    av_dict_set(&opts, "probesize", probe_buf, 0);
    av_dict_set(&opts, "analyzeduration", analyze_buf, 0);
    av_dict_set(&opts, "fflags", live_input ? "discardcorrupt" : "nobuffer+fastseek+discardcorrupt", 0);
    if (live_input) {
        av_dict_set(&opts, "live_start_index", "-3", 0);
        av_dict_set(&opts, "allowed_extensions", "ALL", 0);
        av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,udp,rtp,rtmp,rtmpe,rtmpte,rtmps,rtmpt,rtmpts,ffrtmphttp,rtsp,rtsps,srt,sap,mms,mmst,mmsh,rist,httpproxy,pipe,subfile,concat,data", 0);
        av_dict_set(&opts, "scan_all_pmts", "1", 0);
        av_dict_set(&opts, "seekable", "0", 0);
        if (req->url && (strstr(req->url, "rtsp://") == req->url || strstr(req->url, "rtsps://") == req->url)) {
            av_dict_set(&opts, "rtsp_transport", "tcp", 0);
            av_dict_set(&opts, "stimeout", "30000000", 0);
        }
        if (req->url && strstr(req->url, "udp://") == req->url) {
            av_dict_set(&opts, "fifo_size", "1000000", 0);
            av_dict_set(&opts, "overrun_nonfatal", "1", 0);
        }
    }
    // Keep the TLS connection to atlas alive across Range fetches. Without
    // this, libavformat tears down + reconnects HTTPS for every byte-range
    // it issues during av_seek_frame — that's a fresh TCP+TLS handshake
    // (~1.5 s through the VPN) per seek. With multiple_requests=1 the
    // demuxer pipelines Range fetches over the same HTTPS keep-alive
    // connection so a prepared worker still gets a live socket when the
    // user finally drags the scrub bar.
    av_dict_set(&opts, "multiple_requests", "1", 0);
    // Drop the HTTP keep-alive idle timeout further down the stack: some
    // hosts close idle sockets at 15 s. seek_input below also re-uses the
    // demuxer's now-cached cluster index, so the second+ seek over the
    // same prepared worker is just one Range fetch (~0.5 s).
    av_dict_set(&opts, "icy", "0", 0);
    if (!live_input) av_dict_set(&opts, "seekable", "1", 0);
    fprintf(stderr, "[d_native_processor] input options live_input=%d probesize=%s analyzeduration=%s fflags=%s reconnect_streamed=%d reconnect_at_eof=%d seekable=%d\n",
            live_input, probe_buf, analyze_buf,
            live_input ? "discardcorrupt" : "nobuffer+fastseek+discardcorrupt",
            1,
            reconnect_at_eof,
            live_input ? 0 : 1);

    c->in_fmt = NULL;
    c->audio_stream_idx = -1;
    c->out_audio_stream_idx = -1;
    double t_open0 = now_seconds();
    int rc = avformat_open_input(&c->in_fmt, req->url, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) BAIL_FMT("avformat_open_err=%d", rc);
    fprintf(stderr, "[d_native_processor] avformat_open_input %.2fs\n", now_seconds() - t_open0);
    double t_info0 = now_seconds();
    if (avformat_find_stream_info(c->in_fmt, NULL) < 0) BAIL("find_stream_info");
    fprintf(stderr, "[d_native_processor] find_stream_info %.2fs\n", now_seconds() - t_info0);
    log_input_streams(c->in_fmt);
    c->video_stream_idx = select_video_stream(c->in_fmt);
    if (c->video_stream_idx < 0) BAIL("no_video");
    c->audio_stream_idx = select_audio_stream(c->in_fmt, c->video_stream_idx);
    fprintf(stderr, "[d_native_processor] streams video=%d audio=%d\n",
            c->video_stream_idx, c->audio_stream_idx);

    AVStream* st = c->in_fmt->streams[c->video_stream_idx];
    fprintf(stderr,
            "[d_native_processor] video stream codec=%s w=%d h=%d time_base=%d/%d avg_fps=%d/%d r_fps=%d/%d bit_rate=%lld\n",
            avcodec_get_name(st->codecpar->codec_id),
            st->codecpar->width, st->codecpar->height,
            st->time_base.num, st->time_base.den,
            st->avg_frame_rate.num, st->avg_frame_rate.den,
            st->r_frame_rate.num, st->r_frame_rate.den,
            (long long)st->codecpar->bit_rate);
    const char* decoder_name = cuvid_decoder_name(st->codecpar->codec_id);
    const AVCodec* dec = decoder_name ? avcodec_find_decoder_by_name(decoder_name) : NULL;
    if (!dec) BAIL_FMT("no_cuvid_decoder codec=%s", avcodec_get_name(st->codecpar->codec_id));
    fprintf(stderr, "[d_native_processor] video decoder selected=%s codec=%s\n",
            decoder_name, avcodec_get_name(st->codecpar->codec_id));

    c->dec_ctx = avcodec_alloc_context3(dec);
    if (!c->dec_ctx) BAIL("dec_ctx_alloc");
    avcodec_parameters_to_context(c->dec_ctx, st->codecpar);
    c->dec_ctx->hw_device_ctx = av_buffer_ref(c->hw_device_ref);
    c->dec_ctx->pkt_timebase = st->time_base;
    AVDictionary* dec_opts = NULL;
    av_dict_set(&dec_opts, "deint", "adaptive", 0);
    av_dict_set(&dec_opts, "surfaces", "16", 0);
    if (avcodec_open2(c->dec_ctx, dec, &dec_opts) < 0) {
        av_dict_free(&dec_opts);
        BAIL("decoder_open");
    }
    av_dict_free(&dec_opts);
    if (c->audio_stream_idx >= 0) {
        if (open_audio_decoder(c) < 0) goto bail;
    }

    c->start_pts_us = 0;
    c->audio_next_pts = 0;
    c->audio_source_base_pts_us = 0;
    c->audio_clock_initialized = 0;
    c->audio_clock_resyncs = 0;
    return 0;
bail:
    return -1;
}

// Per-stream timestamp seek. On remote MKVs over the VPN this normally
// costs 5-6 s the first time (libavformat fetches the cluster index via
// several HTTP Range round trips), but the index is then cached in the
// demuxer for the life of the AVFormatContext. bake_prepare issues a
// dummy seek to a mid-file timestamp right after open_input so the
// cluster index is preloaded — every real viewer seek after that is
// just one cached lookup + one Range fetch (~1 s).
static int seek_input(BakeCtx* c, double start_seconds) {
    if (start_seconds <= 0.0) {
        c->start_pts_us = 0;
        c->audio_next_pts = 0;
        c->audio_source_base_pts_us = 0;
        c->audio_clock_initialized = 0;
        c->audio_clock_resyncs = 0;
        return 0;
    }
    double t_seek0 = now_seconds();
    AVStream* vs = c->in_fmt->streams[c->video_stream_idx];
    int64_t target = av_rescale_q((int64_t)(start_seconds * AV_TIME_BASE),
                                   (AVRational){1, AV_TIME_BASE}, vs->time_base);
    if (av_seek_frame(c->in_fmt, c->video_stream_idx, target, AVSEEK_FLAG_BACKWARD) < 0) {
        av_seek_frame(c->in_fmt, -1,
                      (int64_t)(start_seconds * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(c->dec_ctx);
    fprintf(stderr, "[d_native_processor] ts_seek to %.1fs took %.2fs\n",
            start_seconds, now_seconds() - t_seek0);
    c->start_pts_us = (int64_t)(start_seconds * 1000000);
    c->audio_next_pts = 0;
    c->audio_source_base_pts_us = 0;
    c->audio_clock_initialized = 0;
    c->audio_clock_resyncs = 0;
    return 0;
}
