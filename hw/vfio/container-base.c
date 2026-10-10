/*
 * VFIO BASE CONTAINER
 *
 * Copyright (C) 2023 Intel Corporation.
 * Copyright Red Hat, Inc. 2023
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/vfio/vfio-container-base.h"
#include "hw/vfio/loom-dma.h"

static int vfio_container_dma_map_full(VFIOContainerBase *bcontainer,
                                       hwaddr iova, ram_addr_t size,
                                       void *vaddr, bool readonly,
                                       const hwaddr *gpa)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);
    int ret = loom_vfio_dma_map(bcontainer, iova, size, &vaddr, readonly);

    if (ret) {
        return ret;
    }
    if (gpa && loom_vfio_dma_enabled()) {
        uint64_t offset = 0;

        if (*gpa + size < *gpa) {
            ret = -EINVAL;
            goto rollback;
        }
        /*
         * Initialize the translated pool before hardware can access it.
         * Bound each synchronization buffer even for large IOMMU leaves.
         */
        while (offset < size) {
            uint64_t len = MIN(size - offset, 64 * MiB);

            ret = loom_vfio_dma_register(bcontainer, iova + offset,
                                         *gpa + offset, len, readonly ? 1 : 0);
            if (ret) {
                goto rollback;
            }
            offset += len;
        }
    }
    g_assert(vioc->dma_map);
    ret = vioc->dma_map(bcontainer, iova, size, vaddr, readonly);
    if (!ret) {
        return 0;
    }
rollback:
    loom_vfio_dma_unmap(bcontainer, iova, size);
    return ret;
}

int vfio_container_dma_map(VFIOContainerBase *bcontainer,
                           hwaddr iova, ram_addr_t size,
                           void *vaddr, bool readonly)
{
    return vfio_container_dma_map_full(bcontainer, iova, size,
                                       vaddr, readonly, NULL);
}

int vfio_container_dma_map_iommu(VFIOContainerBase *bcontainer,
                                 hwaddr iova, hwaddr gpa, ram_addr_t size,
                                 void *vaddr, bool readonly)
{
    return vfio_container_dma_map_full(bcontainer, iova, size,
                                       vaddr, readonly, &gpa);
}

int vfio_container_dma_unmap(VFIOContainerBase *bcontainer,
                             hwaddr iova, ram_addr_t size,
                             IOMMUTLBEntry *iotlb)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);

    int ret;

    g_assert(vioc->dma_unmap);
    ret = vioc->dma_unmap(bcontainer, iova, size, iotlb);
    if (!ret) {
        ret = loom_vfio_dma_unmap(bcontainer, iova, size);
    }
    return ret;
}

bool vfio_container_add_section_window(VFIOContainerBase *bcontainer,
                                       MemoryRegionSection *section,
                                       Error **errp)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);

    if (!vioc->add_window) {
        return true;
    }

    return vioc->add_window(bcontainer, section, errp);
}

void vfio_container_del_section_window(VFIOContainerBase *bcontainer,
                                       MemoryRegionSection *section)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);

    if (!vioc->del_window) {
        return;
    }

    return vioc->del_window(bcontainer, section);
}

int vfio_container_set_dirty_page_tracking(VFIOContainerBase *bcontainer,
                                           bool start, Error **errp)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);

    if (!bcontainer->dirty_pages_supported) {
        return 0;
    }

    g_assert(vioc->set_dirty_page_tracking);
    return vioc->set_dirty_page_tracking(bcontainer, start, errp);
}

int vfio_container_query_dirty_bitmap(const VFIOContainerBase *bcontainer,
                   VFIOBitmap *vbmap, hwaddr iova, hwaddr size, Error **errp)
{
    VFIOIOMMUClass *vioc = VFIO_IOMMU_GET_CLASS(bcontainer);

    g_assert(vioc->query_dirty_bitmap);
    return vioc->query_dirty_bitmap(bcontainer, vbmap, iova, size,
                                               errp);
}

static gpointer copy_iova_range(gconstpointer src, gpointer data)
{
     Range *source = (Range *)src;
     Range *dest = g_new(Range, 1);

     range_set_bounds(dest, range_lob(source), range_upb(source));
     return dest;
}

GList *vfio_container_get_iova_ranges(const VFIOContainerBase *bcontainer)
{
    assert(bcontainer);
    return g_list_copy_deep(bcontainer->iova_ranges, copy_iova_range, NULL);
}

static void vfio_container_instance_finalize(Object *obj)
{
    VFIOContainerBase *bcontainer = VFIO_IOMMU(obj);
    VFIOGuestIOMMU *giommu, *tmp;

    QLIST_REMOVE(bcontainer, next);

    QLIST_FOREACH_SAFE(giommu, &bcontainer->giommu_list, giommu_next, tmp) {
        memory_region_unregister_iommu_notifier(
                MEMORY_REGION(giommu->iommu_mr), &giommu->n);
        QLIST_REMOVE(giommu, giommu_next);
        g_free(giommu);
    }

    loom_vfio_dma_free(bcontainer);
    g_list_free_full(bcontainer->iova_ranges, g_free);
}

static void vfio_container_instance_init(Object *obj)
{
    VFIOContainerBase *bcontainer = VFIO_IOMMU(obj);

    bcontainer->error = NULL;
    bcontainer->dirty_pages_supported = false;
    bcontainer->dma_max_mappings = 0;
    bcontainer->iova_ranges = NULL;
    QLIST_INIT(&bcontainer->giommu_list);
    QLIST_INIT(&bcontainer->vrdl_list);
}

static const TypeInfo types[] = {
    {
        .name = TYPE_VFIO_IOMMU,
        .parent = TYPE_OBJECT,
        .instance_init = vfio_container_instance_init,
        .instance_finalize = vfio_container_instance_finalize,
        .instance_size = sizeof(VFIOContainerBase),
        .class_size = sizeof(VFIOIOMMUClass),
        .abstract = true,
    },
};

DEFINE_TYPES(types)
