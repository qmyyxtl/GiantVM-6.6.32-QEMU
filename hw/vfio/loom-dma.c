/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hardware DMA remains local to its VFIO owner. The guest DMA API registers
 * buffers; BAR submission and IRQ delivery merge changed bytes through DSM.
 * No hardware DMA mapping points at DSM-managed guest RAM in this mode.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/atomic.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/vfio/loom-dma.h"
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
    uint8_t *baseline, *scratch;
} LoomDMABuffer;

struct LoomVFIODMA {
    GList *maps, *buffers;
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
    *vaddr = m->pool;
    return 0;
}

void loom_vfio_dma_unmap(VFIOContainerBase *bc, hwaddr iova, uint64_t size)
{
    if (!bc->loom_dma) {
        return;
    }
    for (GList *p = bc->loom_dma->maps; p; p = p->next) {
        LoomDMAMap *m = p->data;
        if (m->iova == iova && m->size == size) {
            bc->loom_dma->maps = g_list_delete_link(bc->loom_dma->maps, p);
            munmap(m->pool, m->size);
            g_free(m);
            return;
        }
    }
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
    while (bc->loom_dma->maps) {
        LoomDMAMap *m = bc->loom_dma->maps->data;
        loom_vfio_dma_unmap(bc, m->iova, m->size);
    }
    g_list_free_full(bc->loom_dma->buffers, buffer_free);
    g_free(bc->loom_dma);
    bc->loom_dma = NULL;
}

/*
 * Merge only bytes changed by the relevant producer since the last exchange.
 * Copying a whole coherent buffer back would overwrite CPU updates to other
 * fields in a descriptor while the device updates its completion flags.
 */
static int sync_buffer(VFIOContainerBase *bc, LoomDMABuffer *b, bool to_device)
{
    uint64_t pos = 0;
    int ret;
    if ((to_device && b->direction == 2) ||
        (!to_device && b->direction == 1)) {
        return 0;
    }
    if (to_device) {
        if (address_space_read(&address_space_memory, b->gpa,
                               MEMTXATTRS_UNSPECIFIED, b->scratch, b->size)
            != MEMTX_OK) {
            return -EIO;
        }
    } else {
        smp_rmb();
        ret = pool_copy(bc->loom_dma, b->iova, b->scratch, b->size, false);
        if (ret) {
            return ret;
        }
        /* Device writes preceding its interrupt precede the DSM publication. */
        smp_rmb();
    }
    if (!memcmp(b->scratch, b->baseline, b->size)) {
        return 0;
    }
    while (pos < b->size) {
        uint64_t start;
        while (pos < b->size && b->scratch[pos] == b->baseline[pos]) {
            pos++;
        }
        start = pos;
        while (pos < b->size && b->scratch[pos] != b->baseline[pos]) {
            pos++;
        }
        if (pos == start) {
            break;
        }
        if (to_device) {
            ret = pool_copy(bc->loom_dma, b->iova + start,
                             b->scratch + start, pos - start, true);
        } else {
            ret = address_space_write(&address_space_memory, b->gpa + start,
                                       MEMTXATTRS_UNSPECIFIED,
                                       b->scratch + start, pos - start)
                  == MEMTX_OK ? 0 : -EIO;
        }
        if (ret) {
            return ret;
        }
        memcpy(b->baseline + start, b->scratch + start, pos - start);
    }
    if (to_device) {
        smp_wmb();
    }
    return 0;
}

int loom_vfio_dma_sync(VFIOContainerBase *bc, bool to_device)
{
    int ret;
    if (!bc || !bc->loom_dma) {
        return 0;
    }
    /* Newer payload mappings precede older coherent descriptor mappings. */
    for (GList *p = bc->loom_dma->buffers; p; p = p->next) {
        ret = sync_buffer(bc, p->data, to_device);
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
    ret = pool_copy(ctx, iova, b->baseline, size, true);
    if (ret) {
        buffer_free(b);
        return ret;
    }
    ctx->buffers = g_list_prepend(ctx->buffers, b);
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
            ctx->buffers = g_list_delete_link(ctx->buffers, entry);
            buffer_free(b);
        }
        return 0;
    case LOOM_DMA_SYNC_CPU:
        return sync_buffer(bc, b, false);
    case LOOM_DMA_SYNC_DEVICE:
        return sync_buffer(bc, b, true);
    default:
        return -EINVAL;
    }
}

uint64_t loom_vfio_dma_count(VFIOContainerBase *bc)
{
    return bc && bc->loom_dma ? g_list_length(bc->loom_dma->buffers) : 0;
}
