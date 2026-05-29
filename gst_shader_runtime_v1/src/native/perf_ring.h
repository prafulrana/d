// ============================================================================
// d perf ring — mem2mem shared-memory telemetry pipe.
//
// Replaces the per-frame stderr `[d_native_processor] progress ...` line. bake.c writes
// fixed 192-byte PerfRingSlot records into an mmap'd file every frame. JS
// reads via pread + Buffer view, drains N..head, bulk-inserts to DuckDB.
//
// Why: stderr text parsing pegged the node event loop at ~100% of one core
// during steady-state playback. A 128-byte memcpy + atomic store_release on
// the writer side has no measurable cost; the reader pays ~one pread per
// drain interval (default 1 s) regardless of frame rate.
//
// Layout (matches JS reader in src/perf-ring-reader.js):
//   offset 0..191     PerfRingHeader (one slot equivalent)
//   offset 192..      slots[slot_count] of PerfRingSlot (192 B each)
//
// Concurrency model: single writer (bake.c worker), single reader (JS
// drain loop). Writer increments head with __atomic_store_n(RELEASE) after
// writing the slot body. Reader does __atomic_load_n(ACQUIRE) on head.
// Ring is overwriting — if the reader falls behind by more than slot_count,
// the oldest unread slots get clobbered (acceptable: drain interval << ring
// capacity at any realistic frame rate).
// ============================================================================
#ifndef DPROC_PERF_RING_H
#define DPROC_PERF_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#define PERF_RING_MAGIC        0x39394B53u   // 'd'
#define PERF_RING_VERSION      1u
#define PERF_RING_SLOT_BYTES   192u
#define PERF_RING_SLOT_COUNT   4096u         // 4096 × 192 B = 768 KiB body

// PerfRingSlot.kind values
#define PERF_KIND_FRAME        1u
#define PERF_KIND_EVENT        2u
#define PERF_KIND_STAGE_BURST  3u
#define PERF_KIND_HEALTH       4u

typedef struct __attribute__((packed, aligned(8))) {
    uint32_t magic;            // PERF_RING_MAGIC
    uint32_t version;          // PERF_RING_VERSION
    uint32_t slot_bytes;       // PERF_RING_SLOT_BYTES
    uint32_t slot_count;       // PERF_RING_SLOT_COUNT
    uint64_t head;             // monotonic writer counter (atomic store_release)
    uint32_t pid;              // writer pid
    uint32_t flags;            // bit0 = writer alive
    uint64_t started_at_ns;    // CLOCK_MONOTONIC at open
    uint8_t  _pad[152];        // pad to 192 B
} PerfRingHeader;
_Static_assert(sizeof(PerfRingHeader) == PERF_RING_SLOT_BYTES, "header size");

typedef struct __attribute__((packed, aligned(8))) {
    uint64_t ts_ns;             // CLOCK_MONOTONIC ns
    uint32_t frame_in;          // n_in
    uint32_t frame_out;         // n_out
    int64_t  in_pts_us;         // upstream PTS (us)
    uint64_t bytes_out;         // cumulative bytes_out
    uint64_t audio_packets;
    uint64_t audio_frames;
    uint64_t audio_maxine_runs;
    uint64_t audio_enc_packets;
    uint64_t audio_bytes_out;
    float    loop_fps;
    float    video_timeline_s;
    float    audio_timeline_s;
    float    av_delta_s;
    float    stage_pre_s;       // CUMULATIVE since worker start — reader takes deltas
    float    stage_vsr_s;
    float    stage_post_s;
    float    stage_temporal_s;
    float    stage_nvof_s;
    float    stage_encode_s;
    float    stage_audio_s;
    float    cpu_user_s;        // getrusage at sample time
    float    cpu_sys_s;
    float    rss_mb;
    uint16_t kind;              // PERF_KIND_*
    uint16_t reserved;
    uint32_t event_code;        // for PERF_KIND_EVENT
    uint64_t cadence_drops;     // cumulative graph/input cadence drops
    uint64_t duplicated_frames; // cumulative held/duplicate frame emissions
    uint64_t synthesized_frames;// cumulative motion/generated frame emissions
    uint64_t source_pts_jitter; // cumulative upstream PTS jitter observations
    uint64_t source_pts_discontinuities; // cumulative upstream PTS jumps
    uint8_t  _pad[16];          // pad to 192 B (room for v2 fields)
} PerfRingSlot;
_Static_assert(sizeof(PerfRingSlot) == PERF_RING_SLOT_BYTES, "slot size");

typedef struct {
    int                fd;
    void*              base;       // mmap base (header + slots)
    size_t             map_size;
    PerfRingHeader*    hdr;
    PerfRingSlot*      slots;      // base + 128
    uint64_t           local_head; // private counter; CAS-free single writer
} PerfRing;

static inline uint64_t perf_ring_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Open + truncate + mmap. Returns 0 on success, -1 on failure.
// Single writer, so we own the file lifecycle.
static inline int perf_ring_open_writer(PerfRing* r, const char* path) {
    memset(r, 0, sizeof(*r));
    r->map_size = (size_t)PERF_RING_SLOT_BYTES * (1u + PERF_RING_SLOT_COUNT);
    r->fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (r->fd < 0) return -1;
    if (ftruncate(r->fd, (off_t)r->map_size) != 0) { close(r->fd); r->fd = -1; return -1; }
    r->base = mmap(NULL, r->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (r->base == MAP_FAILED) { close(r->fd); r->fd = -1; r->base = NULL; return -1; }
    r->hdr = (PerfRingHeader*)r->base;
    r->slots = (PerfRingSlot*)((uint8_t*)r->base + PERF_RING_SLOT_BYTES);
    memset(r->base, 0, r->map_size);
    r->hdr->magic = PERF_RING_MAGIC;
    r->hdr->version = PERF_RING_VERSION;
    r->hdr->slot_bytes = PERF_RING_SLOT_BYTES;
    r->hdr->slot_count = PERF_RING_SLOT_COUNT;
    r->hdr->pid = (uint32_t)getpid();
    r->hdr->flags = 1u;
    r->hdr->started_at_ns = perf_ring_now_ns();
    __atomic_store_n(&r->hdr->head, 0ull, __ATOMIC_RELEASE);
    r->local_head = 0;
    return 0;
}

// Reserve next slot for write, return pointer. Caller fills then publishes.
static inline PerfRingSlot* perf_ring_reserve(PerfRing* r) {
    if (!r->base) return NULL;
    return &r->slots[r->local_head % PERF_RING_SLOT_COUNT];
}

// Publish the slot reserved above. Atomic store_release on header head.
static inline void perf_ring_publish(PerfRing* r) {
    if (!r->base) return;
    r->local_head++;
    __atomic_store_n(&r->hdr->head, r->local_head, __ATOMIC_RELEASE);
}

// Mark writer dead + flush. msync covers the case of an abrupt exit where
// tmpfs page writeback never happens (rare for tmpfs but safe to call).
static inline void perf_ring_close(PerfRing* r) {
    if (!r->base) return;
    r->hdr->flags = 0u;
    msync(r->base, r->map_size, MS_ASYNC);
    munmap(r->base, r->map_size);
    if (r->fd >= 0) close(r->fd);
    memset(r, 0, sizeof(*r));
}

#endif
