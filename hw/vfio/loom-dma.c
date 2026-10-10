/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hardware DMA remains local to its VFIO owner. Standard IOMMU notifications
 * (or the legacy DMA bridge) register translated buffers. BAR submission and
 * IRQ delivery merge changed bytes through DSM.
 * No hardware DMA mapping points at DSM-managed guest RAM in this mode.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/atomic.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/vfio/loom-dma.h"
#include "hw/vfio/loom-iavf.h"
#include "sysemu/kvm.h"

typedef struct LoomDMAMap {
    hwaddr iova;
    uint64_t size;
    uint8_t *pool;
    bool readonly;
    GList *link;
} LoomDMAMap;

typedef struct LoomDMABuffer {
    hwaddr iova, gpa;
    uint64_t size;
    unsigned direction, references;
    uint8_t *baseline, *scratch, *pool;
    bool snapshot_ready;
    struct LoomDMABuffer *snapshot_next;
    GList *link;
    uint64_t registration_sequence;
    uint64_t generation;
} LoomDMABuffer;

struct LoomVFIODMA {
    GList *maps, *buffers, *buffers_tail;
    uint8_t *read_buffer;
    uint64_t epoch;
    bool page_indexed;
    GHashTable *page_maps, *page_buffers;
    uint64_t registration_sequence;
    GList *large_maps;
    bool recycle_pages;
    unsigned free_page_count;
    unsigned free_page_limit;
    uint8_t *free_pages[4096];
    bool recycle_buffers;
    unsigned free_buffer_count;
    LoomDMABuffer *free_buffers[64];
};

/*
 * Native guest VT-d buffers describe disjoint 4 KiB pages. Index these pages
 * while retaining a separate list for larger mappings (including boot-time
 * identity and device-memory ranges). Non-page buffers use the general
 * ordered lists until invalidation removes the incompatible layout.
 */
static gpointer page_key(hwaddr iova)
{
    return (gpointer)(uintptr_t)(iova / 4096);
}

static void page_index_disable(LoomVFIODMA *ctx)
{
    ctx->page_indexed = false;
    g_clear_pointer(&ctx->page_maps, g_hash_table_destroy);
    g_clear_pointer(&ctx->page_buffers, g_hash_table_destroy);
}

static void page_index_layout(LoomVFIODMA *ctx, hwaddr iova, uint64_t size)
{
    if (ctx->page_indexed && ((iova & 4095) || size != 4096)) {
        page_index_disable(ctx);
    }
}

static bool map_is_page(const LoomDMAMap *m)
{
    return !(m->iova & 4095) && m->size == 4096;
}

/*
 * Reuse only owner-local staging pages, after the caller has removed the
 * hardware IOMMU mapping and invalidation has retired buffer/queue state.
 * Hardware mappings and registration generations are never cached. Bound the
 * idle storage defaults to 64 pages (256 KiB) per container and has a
 * configurable limit of at most 4096 pages; larger/partial unmaps retain
 * their existing mmap lifetime. Clear every reused page before registration.
 */
static uint8_t *pool_alloc(LoomVFIODMA *ctx, uint64_t size)
{
    uint8_t *page;

    if (ctx->recycle_pages && size == 4096 && ctx->free_page_count) {
        page = ctx->free_pages[--ctx->free_page_count];
        memset(page, 0, 4096);
        return page;
    }
    return mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void pool_retire_page(LoomVFIODMA *ctx, LoomDMAMap *m)
{
    if (ctx->recycle_pages && map_is_page(m) &&
        ctx->free_page_count < ctx->free_page_limit) {
        ctx->free_pages[ctx->free_page_count++] = m->pool;
    } else {
        munmap(m->pool, m->size);
    }
}

static void map_index_add(LoomVFIODMA *ctx, LoomDMAMap *m)
{
    if (!map_is_page(m)) {
        ctx->large_maps = g_list_prepend(ctx->large_maps, m);
    } else if (ctx->page_indexed) {
        g_hash_table_insert(ctx->page_maps, page_key(m->iova), m);
    }
}

static void map_index_remove(LoomVFIODMA *ctx, LoomDMAMap *m)
{
    if (!map_is_page(m)) {
        ctx->large_maps = g_list_remove(ctx->large_maps, m);
    } else if (ctx->page_indexed) {
        g_hash_table_remove(ctx->page_maps, page_key(m->iova));
    }
}

static LoomDMAMap *map_at(LoomVFIODMA *ctx, hwaddr iova)
{
    GList *list = ctx->maps;

    if (ctx->page_indexed) {
        LoomDMAMap *m = g_hash_table_lookup(ctx->page_maps, page_key(iova));

        if (m) {
            return m;
        }
        list = ctx->large_maps;
    }
    for (GList *p = list; p; p = p->next) {
        LoomDMAMap *m = p->data;
        if (iova >= m->iova && iova - m->iova < m->size) {
            return m;
        }
    }
    return NULL;
}

static LoomDMABuffer *buffer_at(LoomVFIODMA *ctx, hwaddr iova)
{
    if (ctx->page_indexed) {
        return g_hash_table_lookup(ctx->page_buffers, page_key(iova));
    }
    for (GList *p = ctx->buffers; p; p = p->next) {
        LoomDMABuffer *b = p->data;
        if (iova >= b->iova && iova - b->iova < b->size) {
            return b;
        }
    }
    return NULL;
}

/*
 * A legacy or non-page buffer layout can temporarily disable page lookups.
 * Once incompatible buffers are invalidated, reconstruct the native indexes.
 * Large maps remain in their separate lookup list. Run after a successful general
 * unmap, where all mapping publications and list mutations have finished.
 */
static void page_index_try_enable(LoomVFIODMA *ctx)
{
    uint64_t sequence = 0;

    if (ctx->page_indexed || sizeof(uintptr_t) != 8 ||
        qemu_real_host_page_size() != 4096) {
        return;
    }
    for (GList *p = ctx->buffers; p; p = p->next) {
        LoomDMABuffer *b = p->data;

        if ((b->iova & 4095) || b->size != 4096) {
            return;
        }
    }
    ctx->page_maps = g_hash_table_new(g_direct_hash, g_direct_equal);
    ctx->page_buffers = g_hash_table_new(g_direct_hash, g_direct_equal);
    ctx->page_indexed = true;
    for (GList *p = ctx->maps; p; p = p->next) {
        LoomDMAMap *m = p->data;

        m->link = p;
        if (!map_is_page(m)) {
            continue;
        }
        if (g_hash_table_contains(ctx->page_maps, page_key(m->iova))) {
            page_index_disable(ctx);
            return;
        }
        g_hash_table_insert(ctx->page_maps, page_key(m->iova), m);
    }
    for (GList *p = ctx->buffers_tail; p; p = p->prev) {
        LoomDMABuffer *b = p->data;

        b->link = p;
        b->registration_sequence = ++sequence;
        if (g_hash_table_contains(ctx->page_buffers, page_key(b->iova))) {
            page_index_disable(ctx);
            return;
        }
        g_hash_table_insert(ctx->page_buffers, page_key(b->iova), b);
    }
    ctx->registration_sequence = sequence;
}

/* Linux enum dma_data_direction: bidirectional=0, to-device=1, from-device=2. */
bool loom_vfio_dma_enabled(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    const char *mode = getenv("LOOM_VFIO_DSM_DMA");
    return mode && !strcmp(mode, "1") && kvm_enabled() &&
           ms->local_cpus != ms->smp.cpus && !ms->shm_path;
}

int loom_vfio_dma_map(VFIOContainerBase *bc, hwaddr iova, uint64_t size,
                     void **vaddr, bool readonly)
{
    LoomDMAMap *m;
    if (!loom_vfio_dma_enabled()) {
        return 0;
    }
    if (!size || iova + size < iova || size > SIZE_MAX) {
        return -EINVAL;
    }
    if (!bc->loom_dma) {
        bc->loom_dma = g_new0(LoomVFIODMA, 1);
        const char *cache_pages = getenv("LOOM_VFIO_POOL_CACHE_PAGES");
        unsigned long cache_limit;

        bc->loom_dma->free_page_limit = 64;
        if (cache_pages) {
            if (qemu_strtoul(cache_pages, NULL, 10, &cache_limit) ||
                cache_limit > G_N_ELEMENTS(bc->loom_dma->free_pages)) {
                error_report("Invalid LOOM_VFIO_POOL_CACHE_PAGES; "
                             "expected 0..4096, using 64");
            } else {
                bc->loom_dma->free_page_limit = cache_limit;
            }
        }
        const char *recycle = getenv("LOOM_VFIO_POOL_RECYCLE");
        bc->loom_dma->recycle_pages = qemu_real_host_page_size() == 4096 &&
                                     (!recycle || strcmp(recycle, "0"));
        const char *buffers = getenv("LOOM_VFIO_BUFFER_RECYCLE");
        bc->loom_dma->recycle_buffers =
            !buffers || strcmp(buffers, "0");
        bc->loom_dma->page_indexed = sizeof(uintptr_t) == 8 &&
                                    qemu_real_host_page_size() == 4096;
        if (bc->loom_dma->page_indexed) {
            bc->loom_dma->page_maps = g_hash_table_new(g_direct_hash,
                                                     g_direct_equal);
            bc->loom_dma->page_buffers = g_hash_table_new(g_direct_hash,
                                                        g_direct_equal);
        }
    }
    bool native_page = !(iova & 4095) && size == 4096;
    if (bc->loom_dma->page_indexed && native_page &&
        map_at(bc->loom_dma, iova)) {
        return -EBUSY;
    }
    for (GList *p = bc->loom_dma->page_indexed && native_page ?
                    NULL : bc->loom_dma->maps;
         p; p = p->next) {
        m = p->data;
        if (iova < m->iova + m->size && m->iova < iova + size) {
            return -EBUSY;
        }
    }
    m = g_new0(LoomDMAMap, 1);
    m->pool = pool_alloc(bc->loom_dma, size);
    if (m->pool == MAP_FAILED) {
        g_free(m);
        return -errno;
    }
    m->iova = iova;
    m->size = size;
    m->readonly = readonly;
    bc->loom_dma->maps = g_list_prepend(bc->loom_dma->maps, m);
    m->link = bc->loom_dma->maps;
    map_index_add(bc->loom_dma, m);
    bc->loom_dma->epoch++;
    *vaddr = m->pool;
    return 0;
}

static int pool_copy(LoomVFIODMA *ctx, hwaddr iova, uint8_t *buf,
                     uint64_t size, bool write)
{
    while (size) {
        LoomDMAMap *m = map_at(ctx, iova);
        uint64_t len;
        if (!m) {
            return -EFAULT;
        }
        len = MIN(size, m->size - (iova - m->iova));
        if (write) {
            memcpy(m->pool + iova - m->iova, buf, len);
        } else {
            memcpy(buf, m->pool + iova - m->iova, len);
        }
        buf += len;
        iova += len;
        size -= len;
    }
    return 0;
}

/*
 * Standard IOMMU buffers fit inside one pool mapping. Cache the host address
 * while registered; range invalidation slices buffers before releasing pool
 * memory. Legacy buffers spanning several mappings retain the lookup path.
 */
static int buffer_pool_copy(LoomVFIODMA *ctx, LoomDMABuffer *b,
                            uint64_t offset, uint8_t *buf,
                            uint64_t size, bool write)
{
    assert(offset <= b->size && size <= b->size - offset);
    if (!b->pool) {
        return pool_copy(ctx, b->iova + offset, buf, size, write);
    }
    if (write) {
        memcpy(b->pool + offset, buf, size);
    } else {
        memcpy(buf, b->pool + offset, size);
    }
    return 0;
}

/*
 * Registration and slicing limit buffers to 64 MiB. Keep metadata, baseline
 * and scratch in one allocation; only metadata needs zero initialization.
 * Borrowed buffer_view() values never own or release this storage.
 */
static LoomDMABuffer *buffer_new(LoomVFIODMA *ctx, uint64_t size)
{
    LoomDMABuffer *b;

    assert(size && size <= 64 * MiB);
    if (ctx && ctx->recycle_buffers && size == 4096 &&
        ctx->free_buffer_count) {
        b = ctx->free_buffers[--ctx->free_buffer_count];
    } else {
        b = g_malloc(sizeof(*b) + 2 * size);
    }
    memset(b, 0, sizeof(*b));
    b->baseline = (uint8_t *)(b + 1);
    b->scratch = b->baseline + size;
    return b;
}

static void buffer_free(gpointer data)
{
    g_free(data);
}

/*
 * Cache allocation storage only, after completion publication, queue
 * invalidation and removal from the live buffer index/list. A fresh
 * registration resets all metadata and reads a new baseline through DSM.
 * General/partial unmaps and borrowed slices keep their original lifetime.
 */
static void buffer_retire(LoomVFIODMA *ctx, LoomDMABuffer *b)
{
    if (ctx->recycle_buffers && b->size == 4096 &&
        ctx->free_buffer_count < G_N_ELEMENTS(ctx->free_buffers)) {
        ctx->free_buffers[ctx->free_buffer_count++] = b;
    } else {
        buffer_free(b);
    }
}

void loom_vfio_dma_free(VFIOContainerBase *bc)
{
    if (!bc->loom_dma) {
        return;
    }
    loom_iavf_dma_invalidate_container(bc, 0, 0);
    for (GList *p = bc->loom_dma->maps; p; p = p->next) {
        LoomDMAMap *m = p->data;

        munmap(m->pool, m->size);
        g_free(m);
    }
    for (unsigned i = 0; i < bc->loom_dma->free_page_count; i++) {
        munmap(bc->loom_dma->free_pages[i], 4096);
    }
    for (unsigned i = 0; i < bc->loom_dma->free_buffer_count; i++) {
        buffer_free(bc->loom_dma->free_buffers[i]);
    }
    g_list_free(bc->loom_dma->maps);
    g_list_free(bc->loom_dma->large_maps);
    g_list_free_full(bc->loom_dma->buffers, buffer_free);
    g_free(bc->loom_dma->read_buffer);
    page_index_disable(bc->loom_dma);
    g_free(bc->loom_dma);
    bc->loom_dma = NULL;
}

/*
 * Merge only bytes changed by the relevant producer since the last exchange.
 * Copying a whole coherent buffer back would overwrite CPU updates to other
 * fields in a descriptor while the device updates its completion flags.
 */

/*
 * Skip whole words only when every byte has the requested equality state.
 * The zero-byte test on XOR detects any unchanged byte; finish a mixed word
 * byte by byte so publication never includes an unchanged destination byte.
 * memcpy keeps unaligned buffers and short tails safe.
 */
static uint64_t skip_matching_bytes(const uint8_t *a, const uint8_t *b,
                                    uint64_t pos, uint64_t size, bool equal)
{
    while (size - pos >= sizeof(uint64_t)) {
        uint64_t x, y;
        memcpy(&x, a + pos, sizeof(x));
        memcpy(&y, b + pos, sizeof(y));
        x ^= y;
        if (equal ? x != 0 :
            ((x - UINT64_C(0x0101010101010101)) & ~x &
             UINT64_C(0x8080808080808080)) != 0) {
            break;
        }
        pos += sizeof(uint64_t);
    }
    while (pos < size && ((a[pos] == b[pos]) == equal)) {
        pos++;
    }
    return pos;
}

static int merge_buffer(VFIOContainerBase *bc, LoomDMABuffer *b,
                         uint8_t *snapshot, bool to_device)
{
    uint64_t pos = 0;
    int ret;
    if (!memcmp(snapshot, b->baseline, b->size)) {
        return 0;
    }
    while (pos < b->size) {
        uint64_t start;
        pos = skip_matching_bytes(snapshot, b->baseline, pos, b->size, true);
        start = pos;
        pos = skip_matching_bytes(snapshot, b->baseline, pos, b->size, false);
        if (pos == start) {
            break;
        }
        if (to_device) {
            ret = buffer_pool_copy(bc->loom_dma, b, start,
                                    snapshot + start, pos - start, true);
        } else {
            ret = address_space_write(&address_space_memory, b->gpa + start,
                                       MEMTXATTRS_UNSPECIFIED,
                                       snapshot + start, pos - start)
                  == MEMTX_OK ? 0 : -EIO;
        }
        if (ret) {
            return ret;
        }
        memcpy(b->baseline + start, snapshot + start, pos - start);
    }
    if (to_device) {
        smp_wmb();
    }
    return 0;
}

static int snapshot_from_device(VFIOContainerBase *bc, LoomDMABuffer *b)
{
    int ret;

    b->snapshot_ready = false;
    b->snapshot_next = NULL;
    if (b->direction == 1) {
        return 0;
    }
    smp_rmb();
    if (b->pool && !memcmp(b->pool, b->baseline, b->size)) {
        smp_rmb();
        return 0;
    }
    ret = buffer_pool_copy(bc->loom_dma, b, 0,
                           b->scratch, b->size, false);
    if (ret) {
        return ret;
    }
    smp_rmb();
    b->snapshot_ready = true;
    return 0;
}

static int sync_buffer(VFIOContainerBase *bc, LoomDMABuffer *b, bool to_device)
{
    int ret;

    if (to_device) {
        if (b->direction == 2) {
            return 0;
        }
        if (address_space_read(&address_space_memory, b->gpa,
                               MEMTXATTRS_UNSPECIFIED, b->scratch, b->size)
            != MEMTX_OK) {
            return -EIO;
        }
    } else {
        ret = snapshot_from_device(bc, b);
        if (ret || !b->snapshot_ready) {
            return ret;
        }
        b->snapshot_ready = false;
    }
    return merge_buffer(bc, b, b->scratch, to_device);
}


/*
 * A temporary view borrows the registered buffer's storage. Only the selected
 * bytes of its baseline are updated; neither registration nor lifetime changes.
 */
static LoomDMABuffer buffer_view(LoomDMABuffer *b, uint64_t offset,
                                 uint64_t size)
{
    LoomDMABuffer part = *b;

    assert(offset <= b->size && size <= b->size - offset);
    part.iova += offset;
    part.gpa += offset;
    part.size = size;
    if (part.pool) {
        part.pool += offset;
    }
    part.baseline += offset;
    part.scratch += offset;
    part.snapshot_ready = false;
    part.snapshot_next = NULL;
    return part;
}

/*
 * Queue submission may touch only part of a mapping or span several mappings.
 * Check the entire registration coverage before publishing any CPU changes.
 * Preserve registration order and every overlapping registration's baseline.
 * The caller must first publish device completions and supply all descriptor
 * and payload ranges transferred back to the device before ringing its tail.
 */
bool loom_vfio_dma_range_covered(VFIOContainerBase *bc, hwaddr iova,
                                 uint64_t size)
{
    hwaddr pos, end;

    if (!bc || !bc->loom_dma || !size || size > 64 * MiB ||
        iova + size < iova) {
        return false;
    }
    end = iova + size;
    if (bc->loom_dma->page_indexed) {
        for (pos = iova; pos < end; ) {
            LoomDMABuffer *b = buffer_at(bc->loom_dma, pos);
            if (!b) {
                return false;
            }
            pos = MIN(end, b->iova + b->size);
        }
        return true;
    }
    for (pos = iova; pos < end; ) {
        hwaddr covered = pos;

        for (GList *p = bc->loom_dma->buffers; p; p = p->next) {
            LoomDMABuffer *b = p->data;

            if (b->iova <= pos && pos - b->iova < b->size) {
                covered = MAX(covered, MIN(end, b->iova + b->size));
            }
        }
        if (covered == pos) {
            return false;
        }
        pos = covered;
    }
    return true;
}

int loom_vfio_dma_sync_to_device_range(VFIOContainerBase *bc, hwaddr iova,
                                       uint64_t size)
{
    LoomVFIODMA *ctx;
    hwaddr end;
    int ret;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!size || size > 64 * MiB || iova + size < iova) {
        return -EINVAL;
    }
    ctx = bc->loom_dma;
    end = iova + size;
    if (!loom_vfio_dma_range_covered(bc, iova, size)) {
        return -ENOENT;
    }
    /*
     * Keep the general path's newest-registration-first publication order,
     * including failure behavior and distinct IOVAs aliasing one guest page.
     * Small native ranges need only a bounded stack array; larger ranges use
     * the existing list rather than allocate a temporary sorting structure.
     */
    if (ctx->page_indexed && (end - 1) / 4096 - iova / 4096 < 16) {
        LoomDMABuffer *selected[16];
        unsigned count = 0;

        for (hwaddr pos = iova; pos < end; ) {
            LoomDMABuffer *b = buffer_at(ctx, pos);
            unsigned n = count++;

            while (n && selected[n - 1]->registration_sequence <
                        b->registration_sequence) {
                selected[n] = selected[n - 1];
                n--;
            }
            selected[n] = b;
            pos = MIN(end, b->iova + b->size);
        }
        for (unsigned n = 0; n < count; n++) {
            LoomDMABuffer *b = selected[n];
            hwaddr start = MAX(iova, b->iova);
            hwaddr stop = MIN(end, b->iova + b->size);
            LoomDMABuffer part = buffer_view(b, start - b->iova, stop - start);

            ret = sync_buffer(bc, &part, true);
            if (ret) {
                return ret;
            }
        }
        return 0;
    }
    for (GList *p = ctx->buffers; p; p = p->next) {
        LoomDMABuffer *b = p->data;
        hwaddr start = MAX(iova, b->iova);
        hwaddr stop = MIN(end, b->iova + b->size);
        LoomDMABuffer part;

        if (start >= stop) {
            continue;
        }
        part = buffer_view(b, start - b->iova, stop - start);
        ret = sync_buffer(bc, &part, true);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static LoomDMABuffer *buffer_slice(LoomDMABuffer *b, uint64_t offset,
                                   uint64_t size)
{
    LoomDMABuffer *part = buffer_new(NULL, size);

    *part = *b;
    part->iova += offset;
    part->gpa += offset;
    part->size = size;
    if (part->pool) {
        part->pool += offset;
    }
    /* Metadata copying must not retain the original buffer's storage. */
    part->baseline = (uint8_t *)(part + 1);
    part->scratch = part->baseline + size;
    memcpy(part->baseline, b->baseline + offset, size);
    part->snapshot_ready = false;
    part->snapshot_next = NULL;
    return part;
}

/*
 * Called only after the backend has stopped DMA to the invalidated range.
 * Publish device writes before dropping staging memory. A range invalidation
 * may cover multiple mappings, or split one mapping into two surviving parts.
 */
/*
 * Preserve page-index lookup and allocation recycling for bounded batched
 * invalidations. Preflight every map and publish all baselines in the general
 * path's newest-registration-first order before modifying either live list.
 */
static int unmap_native_pages(VFIOContainerBase *bc, hwaddr iova,
                               uint64_t size, bool *handled)
{
    LoomVFIODMA *ctx = bc->loom_dma;
    LoomDMABuffer *buffers[16];
    LoomDMAMap *maps[16];
    unsigned pages, count = 0;
    int ret;

    *handled = false;
    if (!ctx->page_indexed || (iova & 4095) || !size ||
        (size & 4095) || size > 16 * 4096ULL) {
        return 0;
    }
    pages = size / 4096;
    for (unsigned i = 0; i < pages; i++) {
        hwaddr address = iova + i * 4096ULL;
        LoomDMABuffer *b = buffer_at(ctx, address);
        LoomDMAMap *m = map_at(ctx, address);

        if (m && !map_is_page(m)) {
            return 0;
        }
        maps[i] = m;
        if (b) {
            unsigned n = count++;

            while (n && buffers[n - 1]->registration_sequence <
                         b->registration_sequence) {
                buffers[n] = buffers[n - 1];
                n--;
            }
            buffers[n] = b;
        }
    }
    *handled = true;
    for (unsigned i = 0; i < count; i++) {
        ret = sync_buffer(bc, buffers[i], false);
        if (ret) {
            return ret;
        }
    }
    loom_iavf_dma_invalidate_container(bc, iova, size);
    for (unsigned i = 0; i < count; i++) {
        LoomDMABuffer *b = buffers[i];

        if (ctx->buffers_tail == b->link) {
            ctx->buffers_tail = b->link->prev;
        }
        g_hash_table_remove(ctx->page_buffers, page_key(b->iova));
        ctx->buffers = g_list_delete_link(ctx->buffers, b->link);
        buffer_retire(ctx, b);
    }
    for (unsigned i = 0; i < pages; i++) {
        LoomDMAMap *m = maps[i];

        if (m) {
            pool_retire_page(ctx, m);
            map_index_remove(ctx, m);
            ctx->maps = g_list_delete_link(ctx->maps, m->link);
            g_free(m);
        }
    }
    ctx->epoch++;
    return 0;
}

int loom_vfio_dma_unmap(VFIOContainerBase *bc, hwaddr iova, uint64_t size)
{
    LoomVFIODMA *ctx = bc->loom_dma;
    GList *p, *next;
    hwaddr end;
    int ret;

    if (!ctx) {
        return 0;
    }
    if (!size || (iova + size < iova && iova + size != 0)) {
        return -EINVAL;
    }
    bool handled;

    ret = unmap_native_pages(bc, iova, size, &handled);
    if (handled) {
        return ret;
    }
    /* A valid invalidation may extend to the end of the address space. */
    end = iova + size ? iova + size : UINT64_MAX;
    for (p = ctx->maps; p; p = p->next) {
        LoomDMAMap *m = p->data;
        hwaddr start = MAX(iova, m->iova);
        hwaddr stop = MIN(end, m->iova + m->size);
        uintptr_t page_size = qemu_real_host_page_size();

        if (start < stop &&
            ((start - m->iova) % page_size ||
             (stop < m->iova + m->size &&
              (stop - m->iova) % page_size))) {
            return -EINVAL;
        }
    }
    /* Finish all publications before altering either list on a read error. */
    for (p = ctx->buffers; p; p = p->next) {
        LoomDMABuffer *b = p->data;
        hwaddr start = MAX(iova, b->iova);
        hwaddr stop = MIN(end, b->iova + b->size);
        LoomDMABuffer part;

        if (start >= stop) {
            continue;
        }
        part = *b;
        part.iova = start;
        part.gpa += start - b->iova;
        part.size = stop - start;
        if (part.pool) {
            part.pool += start - b->iova;
        }
        part.baseline += start - b->iova;
        part.scratch += start - b->iova;
        ret = sync_buffer(bc, &part, false);
        if (ret) {
            return ret;
        }
    }
    loom_iavf_dma_invalidate_container(bc, iova, size);
    for (p = ctx->buffers; p; p = next) {
        LoomDMABuffer *b = p->data;
        hwaddr start = MAX(iova, b->iova);
        hwaddr stop = MIN(end, b->iova + b->size);

        next = p->next;
        if (start >= stop) {
            continue;
        }
        /* Keep surviving slices at the original registration position. */
        if (start > b->iova) {
            ctx->buffers = g_list_insert_before(ctx->buffers, p,
                buffer_slice(b, 0, start - b->iova));
        }
        if (stop < b->iova + b->size) {
            ctx->buffers = g_list_insert_before(ctx->buffers, p,
                buffer_slice(b, stop - b->iova, b->iova + b->size - stop));
        }
        if (ctx->buffers_tail == p) {
            ctx->buffers_tail = p->prev;
        }
        if (ctx->page_indexed) {
            g_hash_table_remove(ctx->page_buffers, page_key(b->iova));
        }
        ctx->buffers = g_list_delete_link(ctx->buffers, p);
        buffer_free(b);
    }
    ctx->epoch++;
    for (p = ctx->maps; p; p = next) {
        LoomDMAMap *m = p->data;
        hwaddr start = MAX(iova, m->iova);
        hwaddr stop = MIN(end, m->iova + m->size);

        next = p->next;
        if (start >= stop) {
            continue;
        }
        map_index_remove(ctx, m);
        if (stop < m->iova + m->size) {
            LoomDMAMap *right = g_new(LoomDMAMap, 1);

            *right = *m;
            right->pool += stop - m->iova;
            right->size -= stop - m->iova;
            right->iova = stop;
            ctx->maps = g_list_prepend(ctx->maps, right);
            right->link = ctx->maps;
            map_index_add(ctx, right);
        }
        munmap(m->pool + start - m->iova, stop - start);
        if (start > m->iova) {
            m->size = start - m->iova;
            map_index_add(ctx, m);
        } else {
            ctx->maps = g_list_delete_link(ctx->maps, p);
            g_free(m);
        }
    }
    page_index_try_enable(ctx);
    return 0;
}

int loom_vfio_dma_sync(VFIOContainerBase *bc, bool to_device)
{
    int ret;
    LoomDMABuffer *ready = NULL;

    if (!bc || !bc->loom_dma) {
        return 0;
    }
    if (to_device) {
        /*
         * Merge adjacent registered GPAs into reads of at most 64 KiB.
         * Keep registration order, read every buffer, and never include gaps.
         * One staging allocation serves all batches without a scatter copy.
         */
        for (GList *p = bc->loom_dma->buffers; p; ) {
            LoomDMABuffer *b = p->data;
            hwaddr lower = b->gpa, upper = b->gpa + b->size;
            GList *end = p->next;

            if (b->direction == 2) {
                p = end;
                continue;
            }
            while (end) {
                LoomDMABuffer *next = end->data;
                hwaddr next_upper = next->gpa + next->size;
                hwaddr new_lower = MIN(lower, next->gpa);
                hwaddr new_upper = MAX(upper, next_upper);

                if (next->direction == 2 ||
                    (next->gpa != upper && next_upper != lower) ||
                    new_upper - new_lower > 64 * KiB) {
                    break;
                }
                lower = new_lower;
                upper = new_upper;
                end = end->next;
            }
            if (p->next == end) {
                ret = sync_buffer(bc, b, true);
                if (ret) {
                    return ret;
                }
                p = end;
                continue;
            }
            if (!bc->loom_dma->read_buffer) {
                bc->loom_dma->read_buffer = g_malloc(64 * KiB);
            }
            if (address_space_read(&address_space_memory, lower,
                                   MEMTXATTRS_UNSPECIFIED,
                                   bc->loom_dma->read_buffer, upper - lower)
                != MEMTX_OK) {
                return -EIO;
            }
            for (; p != end; p = p->next) {
                b = p->data;
                ret = merge_buffer(bc, b,
                                    bc->loom_dma->read_buffer + b->gpa - lower,
                                    true);
                if (ret) {
                    return ret;
                }
            }
        }
        return 0;
    }
    /*
     * Coherent descriptors are registered before their streaming payloads.
     * Capture older descriptor state first, then the newer payload buffers;
     * the DMA read barriers order payload snapshots after observed completion.
     * Publish in the opposite order, using only these snapshots.  Resampling
     * a descriptor after publishing its payload can expose a newly completed
     * descriptor whose DMA payload arrived after that payload's scan.
     */
    /*
     * Snapshot oldest first, prepend changed buffers, then publish newest
     * first. Publication visits only changed snapshots without allocation.
     */
    for (GList *p = bc->loom_dma->buffers_tail; p; p = p->prev) {
        LoomDMABuffer *b = p->data;

        ret = snapshot_from_device(bc, b);
        if (ret) {
            return ret;
        }
        if (b->snapshot_ready) {
            b->snapshot_next = ready;
            ready = b;
        }
    }
    while (ready) {
        LoomDMABuffer *b = ready;

        ready = b->snapshot_next;
        b->snapshot_next = NULL;
        b->snapshot_ready = false;
        ret = merge_buffer(bc, b, b->scratch, false);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

/*
 * Guest physical memory and device-visible IOVA need not be identical.
 * Standard IOMMU notifications can register the translated GPA here; the
 * legacy bridge remains an identity-address caller until it is retired.
 */
int loom_vfio_dma_register(VFIOContainerBase *bc, hwaddr iova, hwaddr gpa,
                           uint64_t size, unsigned direction)
{
    LoomVFIODMA *ctx;
    LoomDMABuffer *b;
    int ret;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!size || size > 64 * MiB || iova + size < iova ||
        gpa + size < gpa || direction > 2) {
        return -EINVAL;
    }
    ctx = bc->loom_dma;
    page_index_layout(ctx, iova, size);
    if (ctx->page_indexed) {
        b = buffer_at(ctx, iova);
        if (b) {
            if (b->direction != direction || b->gpa != gpa) {
                return -EINVAL;
            }
            b->references++;
            return 0;
        }
    }
    for (GList *p = ctx->page_indexed ? NULL : ctx->buffers; p; p = p->next) {
        b = p->data;
        if (b->iova == iova && b->size == size) {
            if (b->direction != direction || b->gpa != gpa) {
                return -EINVAL;
            }
            b->references++;
            return 0;
        }
        /* Conflicting translations must not share device-visible bytes. */
        if (iova < b->iova + b->size && b->iova < iova + size &&
            gpa - iova != b->gpa - b->iova) {
            return -EBUSY;
        }
    }
    b = buffer_new(ctx, size);
    b->iova = iova;
    b->gpa = gpa;
    b->size = size;
    b->direction = direction;
    b->references = 1;
    if (address_space_read(&address_space_memory, gpa,
                           MEMTXATTRS_UNSPECIFIED, b->baseline, size)
        != MEMTX_OK) {
        buffer_free(b);
        return -EIO;
    }
    LoomDMAMap *m = map_at(ctx, iova);
    if (m && size <= m->size && iova - m->iova <= m->size - size) {
        b->pool = m->pool + iova - m->iova;
    }
    ret = buffer_pool_copy(ctx, b, 0, b->baseline, size, true);
    if (ret) {
        buffer_free(b);
        return ret;
    }
    loom_iavf_dma_invalidate_container(bc, iova, size);
    ctx->epoch++;
    b->generation = ctx->epoch;
    ctx->buffers = g_list_prepend(ctx->buffers, b);
    b->link = ctx->buffers;
    if (ctx->registration_sequence == UINT64_MAX) {
        page_index_disable(ctx);
    } else {
        b->registration_sequence = ++ctx->registration_sequence;
    }
    if (ctx->page_indexed) {
        g_hash_table_insert(ctx->page_buffers, page_key(iova), b);
    }
    if (!ctx->buffers_tail) {
        ctx->buffers_tail = ctx->buffers;
    }
    return 0;
}

int loom_vfio_dma_command(VFIOContainerBase *bc, unsigned command,
                         hwaddr iova, uint64_t size, unsigned direction)
{
    LoomVFIODMA *ctx;
    LoomDMABuffer *b;
    GList *entry = NULL;
    int ret;
    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!size || size > 64 * MiB || iova + size < iova || direction > 2) {
        return -EINVAL;
    }
    if (command == LOOM_DMA_REGISTER) {
        return loom_vfio_dma_register(bc, iova, iova, size, direction);
    }
    ctx = bc->loom_dma;
    if (ctx->page_indexed) {
        b = buffer_at(ctx, iova);
        if (b && ((b->iova == iova && b->size == size) ||
            ((command == LOOM_DMA_SYNC_CPU || command == LOOM_DMA_SYNC_DEVICE) &&
             size <= b->size && iova - b->iova <= b->size - size))) {
            entry = b->link;
        }
    }
    for (GList *p = ctx->page_indexed ? NULL : ctx->buffers; p; p = p->next) {
        b = p->data;
        if (b->iova == iova && b->size == size) {
            entry = p;
            break;
        }
    }
    /* Range sync requests may cover a subsection of a streaming mapping. */
    if (!ctx->page_indexed && !entry && (command == LOOM_DMA_SYNC_CPU ||
                   command == LOOM_DMA_SYNC_DEVICE)) {
        for (GList *p = ctx->buffers; p; p = p->next) {
            b = p->data;
            if (iova >= b->iova && size <= b->size &&
                iova - b->iova <= b->size - size) {
                entry = p;
                break;
            }
        }
    }
    if (!entry) {
        return -ENOENT;
    }
    b = entry->data;
    switch (command) {
    case LOOM_DMA_UNREGISTER:
        ret = sync_buffer(bc, b, false);
        if (ret) {
            return ret;
        }
        if (!--b->references) {
            loom_iavf_dma_invalidate_container(bc, iova, size);
            ctx->epoch++;
            if (ctx->buffers_tail == entry) {
                ctx->buffers_tail = entry->prev;
            }
            if (ctx->page_indexed) {
                g_hash_table_remove(ctx->page_buffers, page_key(b->iova));
            }
            ctx->buffers = g_list_delete_link(ctx->buffers, entry);
            buffer_free(b);
        }
        return 0;
    case LOOM_DMA_SYNC_CPU:
    case LOOM_DMA_SYNC_DEVICE: {
        LoomDMABuffer part = buffer_view(b, iova - b->iova, size);

        return sync_buffer(bc, &part, command == LOOM_DMA_SYNC_DEVICE);
    }
    default:
        return -EINVAL;
    }
}

uint64_t loom_vfio_dma_count(VFIOContainerBase *bc)
{
    return bc && bc->loom_dma ? g_list_length(bc->loom_dma->buffers) : 0;
}

uint64_t loom_vfio_dma_epoch(VFIOContainerBase *bc)
{
    return bc && bc->loom_dma ? bc->loom_dma->epoch : 0;
}

/* Read translated CPU-owned bytes without publishing them to hardware. */
int loom_vfio_dma_read_cpu(VFIOContainerBase *bc, hwaddr iova,
                           void *buffer, uint64_t size)
{
    uint8_t *out = buffer;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!size || size > 64 * MiB || iova + size < iova) {
        return -EINVAL;
    }
    while (size) {
        LoomDMABuffer *found = buffer_at(bc->loom_dma, iova);
        uint64_t offset, len;
        if (!found) {
            return -ENOENT;
        }
        offset = iova - found->iova;
        len = MIN(size, found->size - offset);
        if (address_space_read(&address_space_memory, found->gpa + offset,
                               MEMTXATTRS_UNSPECIFIED, out, len) != MEMTX_OK) {
            return -EIO;
        }
        out += len;
        iova += len;
        size -= len;
    }
    return 0;
}

/* Read QEMU's already-published baseline, never the live DMA pool. */
int loom_vfio_dma_read_baseline(VFIOContainerBase *bc, hwaddr iova,
                                void *buffer, uint64_t size)
{
    uint8_t *out = buffer;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!size || size > 64 * MiB || iova + size < iova) {
        return -EINVAL;
    }
    while (size) {
        LoomDMABuffer *found = buffer_at(bc->loom_dma, iova);
        uint64_t offset, len;
        if (!found) {
            return -ENOENT;
        }
        offset = iova - found->iova;
        len = MIN(size, found->size - offset);
        memcpy(out, found->baseline + offset, len);
        out += len;
        iova += len;
        size -= len;
    }
    return 0;
}

/*
 * Read device staging bytes without publishing them or sampling guest RAM.
 * Queue-aware callers capture completion descriptors before reading payloads.
 */
int loom_vfio_dma_read_pool(VFIOContainerBase *bc, hwaddr iova,
                            void *destination, uint64_t size)
{
    int ret;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!destination || !size || iova + size < iova) {
        return -EINVAL;
    }
    if (!loom_vfio_dma_range_covered(bc, iova, size)) {
        return -ENOENT;
    }
    smp_rmb();
    ret = pool_copy(bc->loom_dma, iova, destination, size, false);
    smp_rmb();
    return ret;
}

/*
 * Publish only a caller-owned immutable snapshot. Never re-read device memory:
 * doing so could expose a late descriptor completion before its payload.
 * Preflight coverage before the first guest write; merge only changed bytes,
 * preserving CPU modifications and producer baselines outside this range.
 */
int loom_vfio_dma_publish_snapshot(VFIOContainerBase *bc, hwaddr iova,
                                   const void *snapshot, uint64_t size)
{
    const uint8_t *bytes = snapshot;
    hwaddr end;
    int ret;

    if (!bc || !bc->loom_dma) {
        return -ENODEV;
    }
    if (!snapshot || !size || iova + size < iova) {
        return -EINVAL;
    }
    if (!loom_vfio_dma_range_covered(bc, iova, size)) {
        return -ENOENT;
    }
    end = iova + size;
    while (iova < end) {
        LoomDMABuffer *b = buffer_at(bc->loom_dma, iova);
        uint64_t len = MIN(end - iova, b->size - (iova - b->iova));
        LoomDMABuffer part = buffer_view(b, iova - b->iova, len);

        if (b->direction != 1) {
            ret = merge_buffer(bc, &part, (uint8_t *)bytes, false);
            if (ret) {
                return ret;
            }
        }
        iova += len;
        bytes += len;
    }
    return 0;
}

/*
 * RX address caches must distinguish an IOVA's registration lifetimes.
 * Zero rejects non-native layouts, missing pages and device-read-only pages.
 * Unlike registration_sequence (which orders publication), this identity is
 * not renumbered when the page index is reconstructed.
 */
uint64_t loom_vfio_dma_page_generation(VFIOContainerBase *bc, hwaddr iova)
{
    LoomDMABuffer *b;

    if (!bc || !bc->loom_dma || !bc->loom_dma->page_indexed) {
        return 0;
    }
    b = buffer_at(bc->loom_dma, iova);
    return b && b->direction != 1 ? b->generation : 0;
}
