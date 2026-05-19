// sagegc.h — SageGC: Generational slab allocator + TLAB + SATB
// 
// Design:
//   - Thread-Local Allocation Buffers (TLABs): each thread bumps into a 
//     private buffer, acquiring new chunks from a central pool with one lock.
//     Zero contention on the hot path.
//
//   - Slab pages: 1 MB pages of same-sized objects. Avoids fragmentation,
//     improves cache locality, makes sweep O(page_count) not O(object_count).
//
//   - Two generations:
//       Young (nursery): 4 MB, bump-pointer, collected every ~1 MB allocated.
//       Old (tenured):   slab pages, collected less frequently.
//     Most allocations die young → short-lived objects never reach tenured.
//
//   - SATB write barrier (Snapshot-At-The-Beginning): concurrent mark phase,
//     brief STW for root scan and remark.
//
//   - Precise stack roots via GC_PUSH/GC_POP macros (already in interpreter).
//
// API (drop-in replacement for gc_alloc/gc_free):
//   void* sgc_alloc(int type, size_t size)   — allocate GC-managed object
//   void  sgc_collect()                       — trigger full collection
//   void  sgc_minor()                         — collect nursery only
//   void  sgc_pin_value(Value* v)             — pin from current thread
//   void  sgc_unpin_value(Value* v)           — unpin
//   SGCStats sgc_stats()                      — performance counters
//
// Thread safety:
//   Hot path (sgc_alloc): lock-free bump into TLAB, one lock per TLAB refill.
//   Slow path (collection): STW for root scan ~50-200µs, all else concurrent.
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "gc.h"  // Keep existing GCHeader for compat

// ── Tuning constants ──────────────────────────────────────────────────────────
#define SGC_NURSERY_SIZE     (4  * 1024 * 1024)  // 4 MB young gen
#define SGC_SLAB_PAGE_SIZE   (1  * 1024 * 1024)  // 1 MB slab page
#define SGC_TLAB_SIZE        (64 * 1024)          // 64 KB per thread TLAB
#define SGC_TLAB_WASTE_MAX   (4  * 1024)          // Waste up to 4KB at TLAB end
#define SGC_PROMO_AGE        2                    // Survive 2 minors → promote
#define SGC_MINOR_TRIGGER    (SGC_NURSERY_SIZE / 4)  // Collect after 1MB allocated
#define SGC_MAJOR_TRIGGER    (32 * 1024 * 1024)  // Major after 32MB tenured

// ── Object header (compacted: 12 bytes → fits in one cache line with payload) ─
typedef struct SGCHeader {
    uint32_t flags;      // [31..24]=type  [23..16]=age  [15..8]=color  [7..0]=flags
    uint32_t size;       // payload size (bytes)
    struct SGCHeader* next_in_page;  // page intrusive list (NULL for last)
} SGCHeader;

#define SGC_HDR_TYPE(h)    (((h)->flags >> 24) & 0xFF)
#define SGC_HDR_AGE(h)     (((h)->flags >> 16) & 0xFF)
#define SGC_HDR_COLOR(h)   (((h)->flags >>  8) & 0xFF)
#define SGC_HDR_FLAGS(h)   ( (h)->flags        & 0xFF)
#define SGC_HDR_SIZE(h)    ((h)->size)
#define SGC_PAYLOAD(h)     ((void*)((h) + 1))
#define SGC_HEADER(p)      (((SGCHeader*)(p)) - 1)

#define SGC_SET_TYPE(h,t)  ((h)->flags = ((h)->flags & ~0xFF000000u) | (((uint32_t)(t) & 0xFF) << 24))
#define SGC_SET_AGE(h,a)   ((h)->flags = ((h)->flags & ~0x00FF0000u) | (((uint32_t)(a) & 0xFF) << 16))
#define SGC_SET_COLOR(h,c) ((h)->flags = ((h)->flags & ~0x0000FF00u) | (((uint32_t)(c) & 0xFF) <<  8))

#define SGC_WHITE  0
#define SGC_GRAY   1
#define SGC_BLACK  2
#define SGC_PINNED 3

// ── TLAB — Thread-Local Allocation Buffer ─────────────────────────────────────
typedef struct SGCTLAB {
    char*    start;          // Buffer start
    char*    cursor;         // Next free byte
    char*    end;            // Buffer end
    size_t   bytes_allocated;// Stats
    size_t   refills;        // How many times we requested a new chunk
    int      thread_id;
} SGCTLAB;

// ── Slab page (for tenured objects) ──────────────────────────────────────────
typedef struct SGCPage {
    char*          data;         // 1 MB aligned data
    size_t         used;         // Bytes used
    size_t         object_count; // Live objects in page
    struct SGCPage* next;
    int            generation;   // 0=nursery, 1=tenured
} SGCPage;

// ── Global GC state ───────────────────────────────────────────────────────────
typedef struct {
    // Nursery (bump-pointer, per-thread TLABs)
    char*    nursery_start;
    char*    nursery_end;
    _Atomic(char*) nursery_cursor;    // atomic bump pointer

    // TLABs (one per thread, up to 64 threads)
    SGCTLAB  tlabs[64];
    int      tlab_count;

    // Tenured slab pages
    SGCPage* pages;
    size_t   tenured_bytes;

    // Collection triggers
    _Atomic(size_t) bytes_allocated_since_minor;
    size_t   bytes_since_major;

    // Stats
    uint64_t minor_collections;
    uint64_t major_collections;
    uint64_t bytes_promoted;
    uint64_t max_pause_ns;

    // SATB mark queue (concurrent mark)
    SGCHeader** mark_queue;
    _Atomic(int) mark_queue_head;
    _Atomic(int) mark_queue_tail;
    int          mark_queue_cap;

    // GC phase (atomic for concurrent access)
    _Atomic(int) phase;
    int          write_barrier_active;

    // Lock (only for: TLAB refill, major STW, page allocation)
    pthread_mutex_t lock;
} SGCState;

extern SGCState sgc;
extern _Thread_local SGCTLAB* sgc_thread_tlab;

// ── Public API ────────────────────────────────────────────────────────────────
void  sgc_init(void);
void  sgc_shutdown(void);
void* sgc_alloc(int type, size_t size);   // hot path — inline bump pointer
void  sgc_minor(void);                    // collect nursery
void  sgc_collect(void);                  // full collection
void  sgc_write_barrier(void** slot, void* new_val);  // SATB barrier

typedef struct {
    size_t nursery_used, nursery_cap;
    size_t tenured_bytes;
    uint64_t minor_collections, major_collections;
    uint64_t bytes_promoted;
    uint64_t max_pause_ns;
} SGCStats;
SGCStats sgc_stats(void);

// ── Fast inline allocation (lock-free TLAB bump) ──────────────────────────────
static inline void* sgc_alloc_fast(int type, size_t size) {
    SGCTLAB* tlab = sgc_thread_tlab;
    if (__builtin_expect(tlab == NULL, 0)) return sgc_alloc(type, size);

    size_t total = sizeof(SGCHeader) + size;
    // Align to 8 bytes
    total = (total + 7u) & ~7u;

    char* new_cursor = tlab->cursor + total;
    if (__builtin_expect(new_cursor > tlab->end, 0)) {
        // TLAB exhausted: slow path refills it
        return sgc_alloc(type, size);
    }

    SGCHeader* h = (SGCHeader*)tlab->cursor;
    tlab->cursor = new_cursor;
    tlab->bytes_allocated += total;

    h->flags = ((uint32_t)type << 24) | (SGC_WHITE << 8);
    h->size  = (uint32_t)size;
    h->next_in_page = NULL;

    atomic_fetch_add(&sgc.bytes_allocated_since_minor, total);
    return SGC_PAYLOAD(h);
}

// Trigger minor if nursery is filling up
#define SGC_ALLOC(type, size) \
    (atomic_load(&sgc.bytes_allocated_since_minor) >= SGC_MINOR_TRIGGER \
     ? (sgc_minor(), sgc_alloc_fast(type, size)) \
     : sgc_alloc_fast(type, size))
