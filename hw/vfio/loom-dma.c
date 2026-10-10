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
} LoomDMAMap;

typedef struct LoomDMABuffer {
    hwaddr iova, gpa;
    uint64_t size;
    unsigned direction, references;
    uint8_t *baseline, *scratch, *pool;
    bool snapshot_ready;
    struct LoomDMABuffer *snapshot_next;
} LoomDMABuffer;

struct LoomVFIODMA {
    GList *maps, *buffers, *buffers_tail;
    uint8_t *read_buffer;
    uint64_t epoch;
};

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
    }
    for (GList *p = bc->loom_dma->maps; p; p = p->next) {
        m = p->data;
        if (iova < m->iova + m->size && m->iova < iova + size) {
            return -EBUSY;
        }
    }
    m = g_new0(LoomDMAMap, 1);
    m->pool = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->pool == MAP_FAILED) {
        g_free(m);
        return -errno;
    }
    m->iova = iova;
    m->size = size;
    m->readonly = readonly;
    bc->loom_dma->maps = g_list_prepend(bc->loom_dma->maps, m);
    bc->loom_dma->epoch++;
    *vaddr = m->pool;
    return 0;
}

static int pool_copy(LoomVFIODMA *ctx, hwaddr iova, uint8_t *buf,
                     uint64_t size, bool write)
{
    while (size) {
        LoomDMAMap *m = NULL;
        uint64_t len;
        for (GList *p = ctx->maps; p; p = p->next) {
            LoomDMAMap *candidate = p->data;
            if (iova >= candidate->iova &&
                iova - candidate->iova < candidate->size) {
                m = candidate;
                break;
            }
        }
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

static void buffer_free(gpointer data)
{
    LoomDMABuffer *b = data;
    g_free(b->scratch);
    g_free(b->baseline);
    g_free(b);
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
    g_list_free(bc->loom_dma->maps);
    g_list_free_full(bc->loom_dma->buffers, buffer_free);
    g_free(bc->loom_dma->read_buffer);
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
    LoomDMABuffer *part = g_new(LoomDMABuffer, 1);

    *part = *b;
    part->iova += offset;
    part->gpa += offset;
    part->size = size;
    if (part->pool) {
        part->pool += offset;
    }
    part->baseline = g_memdup2(b->baseline + offset, size);
    part->scratch = g_malloc(size);
    part->snapshot_ready = false;
    part->snapshot_next = NULL;
    return part;
}

/*
 * Called only after the backend has stopped DMA to the invalidated range.
 * Publish device writes before dropping staging memory. A range invalidation
 * may cover multiple mappings, or split one mapping into two surviving parts.
 */
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
        ctx->buffers = g_list_delete_link(ctx->buffers, p);
        buffer_free(b);
    }
    ctx->epoch++;
    ctx->buffers_tail = g_list_last(ctx->buffers);
    for (p = ctx->maps; p; p = next) {
        LoomDMAMap *m = p->data;
        hwaddr start = MAX(iova, m->iova);
        hwaddr stop = MIN(end, m->iova + m->size);

        next = p->next;
        if (start >= stop) {
            continue;
        }
        if (stop < m->iova + m->size) {
            LoomDMAMap *right = g_new(LoomDMAMap, 1);

            *right = *m;
            right->pool += stop - m->iova;
            right->size -= stop - m->iova;
            right->iova = stop;
            ctx->maps = g_list_prepend(ctx->maps, right);
        }
        munmap(m->pool + start - m->iova, stop - start);
        if (start > m->iova) {
            m->size = start - m->iova;
        } else {
            ctx->maps = g_list_delete_link(ctx->maps, p);
            g_free(m);
        }
    }
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
    for (GList *p = ctx->buffers; p; p = p->next) {
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
    b = g_new0(LoomDMABuffer, 1);
    b->iova = iova;
    b->gpa = gpa;
    b->size = size;
    b->direction = direction;
    b->references = 1;
    b->baseline = g_malloc(size);
    b->scratch = g_malloc(size);
    if (address_space_read(&address_space_memory, gpa,
                           MEMTXATTRS_UNSPECIFIED, b->baseline, size)
        != MEMTX_OK) {
        buffer_free(b);
        return -EIO;
    }
    for (GList *p = ctx->maps; p; p = p->next) {
        LoomDMAMap *m = p->data;

        if (iova >= m->iova && size <= m->size &&
            iova - m->iova <= m->size - size) {
            b->pool = m->pool + iova - m->iova;
            break;
        }
    }
    ret = buffer_pool_copy(ctx, b, 0, b->baseline, size, true);
    if (ret) {
        buffer_free(b);
        return ret;
    }
    loom_iavf_dma_invalidate_container(bc, iova, size);
    ctx->epoch++;
    ctx->buffers = g_list_prepend(ctx->buffers, b);
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
    for (GList *p = ctx->buffers; p; p = p->next) {
        b = p->data;
        if (b->iova == iova && b->size == size) {
            entry = p;
            break;
        }
    }
    /* Range sync requests may cover a subsection of a streaming mapping. */
    if (!entry && (command == LOOM_DMA_SYNC_CPU ||
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
        LoomDMABuffer *found = NULL;
        uint64_t offset, len;

        for (GList *p = bc->loom_dma->buffers; p; p = p->next) {
            LoomDMABuffer *b = p->data;

            if (iova >= b->iova && iova - b->iova < b->size) {
                found = b;
                break;
            }
        }
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
        LoomDMABuffer *found = NULL;
        uint64_t offset, len;

        for (GList *p = bc->loom_dma->buffers; p; p = p->next) {
            LoomDMABuffer *b = p->data;

            if (iova >= b->iova && iova - b->iova < b->size) {
                found = b;
                break;
            }
        }
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
