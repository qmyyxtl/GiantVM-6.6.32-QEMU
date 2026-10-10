/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Authorize physical guest RAM without eagerly mapping the entire domain. */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "exec/address-spaces.h"
#include "hw/vfio/vfio-common.h"
#include "hw/vfio/loom-dma.h"

int loom_vfio_physical_mode(void)
{
    const char *mode = getenv("LOOM_VFIO_PHYSICAL_DMA");

    if (!mode || !strcmp(mode, "0")) {
        return 0;
    }
    return !strcmp(mode, "1") ? 1 : -EINVAL;
}

int loom_vfio_physical_attach(VFIODevice *dev, AddressSpace *as)
{
    int mode = loom_vfio_physical_mode();

    if (mode <= 0) {
        return mode;
    }
    if (!loom_vfio_dma_enabled() || as != &address_space_memory ||
        dev->type != VFIO_DEVICE_TYPE_PCI || dev->mdev || dev->iommufd ||
        qemu_real_host_page_size() != 4096) {
        return -ENOTSUP;
    }
    return 1;
}

int loom_vfio_physical_region(VFIOContainerBase *bc,
                               MemoryRegionSection *section, bool add)
{
    MemoryRegion *mr = section->mr;
    Int128 limit;
    uint64_t start, end;
    int ret;

    assert(bql_locked());
    if (!bc->loom_physical) {
        return 0;
    }
    if (!add && !bc->initialized) {
        /* Initialization failure: some add callbacks never installed authority.
         * No descriptor may be exposed before realization finishes. Retain
         * metadata for container destruction instead of revoking guessed spans.
         */
        return loom_vfio_dma_count(bc) ? -EBUSY : 1;
    }
    if (!bc->space || bc->space->as != &address_space_memory ||
        qemu_real_host_page_size() != 4096) {
        return -ENOTSUP;
    }
    if (memory_region_is_iommu(mr)) {
        return -ENOTSUP; /* Guest DMA must address physical RAM, not a gIOMMU. */
    }
    if (!memory_region_is_ram(mr) || memory_region_is_ram_device(mr)) {
        return 1; /* MMIO and RAMDevice overlays are never DMA authority. */
    }
    if (memory_region_is_protected(mr) ||
        memory_region_has_ram_discard_manager(mr)) {
        return -ENOTSUP;
    }
    if ((section->offset_within_address_space ^
         section->offset_within_region) & 4095) {
        return -EINVAL;
    }
    limit = int128_add(int128_make64(section->offset_within_address_space),
                       section->size);
    if (int128_le(section->size, int128_zero()) ||
        int128_gt(limit, int128_make64(UINT64_MAX)) ||
        section->offset_within_address_space > UINT64_MAX - 4095) {
        return -EINVAL;
    }
    start = (section->offset_within_address_space + 4095) & ~UINT64_C(4095);
    end = int128_get64(limit) & ~UINT64_C(4095);
    if (start >= end) {
        return 1; /* A partial page is not authority for an entire DMA page. */
    }
    if (add) {
        ret = loom_vfio_ram_add(bc, start, end - start, section->readonly);
    } else {
        /*
         * VM pause/BQL does not stop hardware DMA. While a device is attached,
         * dynamic RAM removal is unsupported after any page materialization.
         * The listener's caller aborts before deleting authorization or pools.
         * Normal legacy teardown closes/removes devices before unregistering
         * the last container's listener; initialization has no DMA maps.
         */
        if (!QLIST_EMPTY(&bc->device_list) && loom_vfio_dma_count(bc)) {
            return -EBUSY;
        }
        ret = loom_vfio_ram_remove(bc, start, end - start);
    }
    return ret ? ret : 1;
}
