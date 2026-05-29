ARG DGST_BASE_IMAGE=nvcr.io/nvidia/deepstream:8.0-triton-multiarch
FROM ${DGST_BASE_IMAGE}

USER root

RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    curl \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libjson-glib-dev \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswresample-dev \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-libav \
    && apt-get install -y --reinstall \
    libvpx9 \
    libmpg123-0t64 \
    libmp3lame0 \
    libflac12t64 \
    libdvdread8t64 \
    libdvdnav4 \
    libmjpegutils-2.1-0t64 \
    libdca0 \
    libx264-164 \
    libx265-199 \
    && rm -rf /var/lib/apt/lists/*

COPY src /opt/dgst/src
COPY graphs /opt/dgst/graphs
COPY models /opt/dgst/models

WORKDIR /opt/dgst
ENV DEEPSTREAM_DIR=/opt/nvidia/deepstream/deepstream-8.0
ENV LD_LIBRARY_PATH=/opt/dgst/src/native/nvvfx/libs:${DEEPSTREAM_DIR}/lib:${LD_LIBRARY_PATH}
ENV CTRL_PORT=8088
ENV DGST_DEFAULT_GRAPH=/opt/dgst/graphs/default_video.json
ENV DGST_MEDIAMTX_RTSP_URL="unix:/run/99ks/99sk.ts.sock"
ENV DGST_PROCESSOR=native

RUN gcc -O2 -pipe -Wall -Wextra \
      -Isrc \
      -o dgst_runtime \
      src/main.c src/app.c src/config.c src/control.c src/graph.c \
      $(pkg-config --cflags --libs glib-2.0 json-glib-1.0) \
    && make -C src/native clean all

EXPOSE 8088

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD curl -fsS http://127.0.0.1:${CTRL_PORT}/v1/status || exit 1

CMD ["/opt/dgst/dgst_runtime"]
