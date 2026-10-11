/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sparse X710 BAR adapter. Sparse RAM authorization remains opt-in/inactive. */
#include "qemu/osdep.h"
#include "hw/vfio/vfio-common.h"
#include "hw/vfio/loom-dma.h"
#include "hw/vfio/loom-iavf.h"
#include "hw/vfio/loom-iavf-admin.h"

#define PHYSICAL_AQ_ENABLE (1U << 31)

typedef struct PhysicalBAR {
    VFIORegion *region;
    hwaddr original;
    uint32_t value;
    bool observed;
} PhysicalBAR;

static bool physical_critical(hwaddr addr, unsigned size)
{
    static const hwaddr reg[] = {
        0x6800, 0x7c00, 0x7800, 0x6400, 0x8400,
        0x8000, 0x6c00, 0x6000, 0x7400, 0x7000,
    };

    if (!size || addr > UINT64_MAX - size) {
        return true;
    }
    if (addr < 0x40 || (addr < 0x2040 && addr + size > 0x2000)) {
        return true;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(reg); i++) {
        if (addr < reg[i] + 4 && addr + size > reg[i]) {
            return true;
        }
    }
    return false;
}

static int physical_raw_write(VFIORegion *r, hwaddr addr, uint32_t value)
{
    uint32_t little = cpu_to_le32(value);
    ssize_t n = pwrite(r->vbasedev->fd, &little, 4, r->fd_offset + addr);

    return n == 4 ? 0 : n < 0 ? -errno : -EIO;
}

static int physical_read(void *opaque, hwaddr addr, uint32_t *value)
{
    PhysicalBAR *p = opaque;
    uint32_t little;
    ssize_t n = pread(p->region->vbasedev->fd, &little, 4,
                       p->region->fd_offset + addr);

    if (n != 4) {
        return n < 0 ? -errno : -EIO;
    }
    *value = le32_to_cpu(little);
    return 0;
}

static int physical_write(void *opaque, hwaddr addr, uint32_t value)
{
    PhysicalBAR *p = opaque;

    /* Tails are observed after preparation, immediately before hardware DMA. */
    if (!p->observed && addr == p->original) {
        loom_iavf_observe_write(p->region->vbasedev, p->original, p->value);
        p->observed = true;
    }
    return physical_raw_write(p->region, addr, value);
}

static void physical_fail(PhysicalBAR *p)
{
    p->region->loom_iavf_admin->failed = true;
    /* Retain pools: enable readback is not proof that outstanding DMA drained. */
    for (unsigned i = 0; i < 2; i++) {
        hwaddr addr = i ? 0x8000 : 0x6800;
        uint32_t value;

        if (!physical_read(p, addr, &value)) {
            physical_raw_write(p->region, addr, value & ~PHYSICAL_AQ_ENABLE);
            physical_read(p, addr, &value);
        }
    }
}

static int physical_data_write(PhysicalBAR *p, hwaddr addr, uint32_t value)
{
    VFIODevice *dev = p->region->vbasedev;
    int ret = loom_iavf_prepare_physical_data(dev, addr, value);

    if (ret == 2) {
        return 1; /* Cached startup hint; no device-visible submission. */
    }
    if (ret != 1) {
        return ret ? ret : -ENOTSUP;
    }
    ret = loom_iavf_record_physical_data(dev, addr, value);
    if (!ret) {
        ret = physical_raw_write(p->region, addr, value);
    }
    if (!ret) {
        ret = loom_iavf_commit_physical_data(dev, addr, value);
    }
    return ret ? ret : 1;
}

static int physical_prepare(void *opaque, bool arq, bool enabling,
                              uint64_t base, unsigned count, unsigned next,
                              unsigned tail)
{
    PhysicalBAR *p = opaque;
    VFIODevice *dev = p->region->vbasedev;
    int ret = loom_iavf_prepare_physical_admin(dev, arq, enabling, base,
                                               count, next, tail);

    if (ret || arq || enabling) {
        return ret;
    }
    ret = loom_iavf_physical_rx_configuration(dev);
    if (ret <= 0) {
        return ret;
    }
    /* CONFIG ACK must precede this flush; PF ENABLE must follow it. */
    for (unsigned id = 0; id < 16; id++) {
        uint32_t value;

        if (!loom_iavf_deferred_physical_rx(dev, id, &value)) {
            continue;
        }
        ret = physical_data_write(p, 0x2000 + id * 4, value);
        if (ret != 1) {
            return ret < 0 ? ret : -EIO;
        }
    }
    return 0;
}

int loom_iavf_physical_bar_write(VFIORegion *r, hwaddr addr,
                                  uint64_t value, unsigned size)
{
    VFIODevice *dev = r->vbasedev;
    PhysicalBAR p = { .region = r, .original = addr, .value = value };
    static const LoomIAVFAdminOps ops = {
        .write = physical_write, .read = physical_read,
        .prepare = physical_prepare,
    };
    bool data;
    int ret;

    if (!dev->bcontainer || !dev->bcontainer->loom_ram) {
        return 0;
    }
    if (dev->type != VFIO_DEVICE_TYPE_PCI || !dev->loom_iavf_irq_control) {
        return -ENOTSUP;
    }
    if (r->loom_iavf_admin && r->loom_iavf_admin->failed) {
        return -EIO;
    }
    if (r->nr || !physical_critical(addr, size)) {
        return 0;
    }
    if (!r->loom_iavf_admin) {
        r->loom_iavf_admin = g_new0(LoomIAVFAdminState, 1);
        for (unsigned i = 0; i < 2; i++) {
            uint32_t length;
            ret = physical_read(&p, i ? 0x8000 : 0x6800, &length);
            if (ret || (length & PHYSICAL_AQ_ENABLE)) {
                ret = ret ? ret : -EBUSY;
                goto fail;
            }
        }
    }
    if (size != 4 || (addr & 3) || value > UINT32_MAX ||
        addr > r->size || r->size - addr < 4) {
        ret = -EINVAL;
        goto fail;
    }
    data = addr <= 0x3c || (addr >= 0x2000 && addr <= 0x203c);
    ret = data ? loom_iavf_sync_data_completion(dev) :
                 loom_iavf_sync_control_write(r, addr, size);
    if (ret < 0) {
        goto fail;
    }
    if (!ret) {
        ret = loom_vfio_dma_sync(dev->bcontainer, false);
        if (ret) {
            goto fail;
        }
        loom_iavf_reconcile_completion(dev);
        loom_iavf_observe_completion(dev);
    }
    if (data) {
        ret = physical_data_write(&p, addr, value);
    } else {
        /* Configuration must be visible to preparation on its third field.
         * Internal physical LEN disables/enables must never overwrite it.
         */
        if (addr != 0x8400 && addr != 0x7000) {
            loom_iavf_observe_write(dev, addr, value);
            p.observed = true;
        }
        ret = loom_iavf_admin_write(r->loom_iavf_admin, addr, value, &ops, &p);
        if (!ret) {
            ret = -ENOTSUP; /* A critical overlap cannot use generic forwarding. */
        }
    }
    if (ret > 0) {
        return 1;
    }
fail:
    physical_fail(&p);
    return ret < 0 ? ret : -EIO;
}

void loom_iavf_physical_bar_read(VFIORegion *r, hwaddr addr,
                                  unsigned size, uint64_t *value)
{
    if (r->nr == 0 && size == 4 && r->loom_iavf_admin) {
        uint32_t v = *value;

        loom_iavf_admin_read(r->loom_iavf_admin, addr, &v);
        *value = v;
    }
}
