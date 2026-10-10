/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VFIO_LOOM_DMA_H
#define HW_VFIO_LOOM_DMA_H
#include "hw/vfio/vfio-container-base.h"
typedef struct LoomVFIODMA LoomVFIODMA;
enum {
    LOOM_DMA_REGISTER = 1, LOOM_DMA_UNREGISTER = 2,
    LOOM_DMA_SYNC_CPU = 3, LOOM_DMA_SYNC_DEVICE = 4,
};
bool loom_vfio_dma_enabled(void);
int loom_vfio_dma_map(VFIOContainerBase *, hwaddr, uint64_t, void **, bool);
void loom_vfio_dma_unmap(VFIOContainerBase *, hwaddr, uint64_t);
void loom_vfio_dma_free(VFIOContainerBase *);
int loom_vfio_dma_register(VFIOContainerBase *bc, hwaddr iova, hwaddr gpa,
                           uint64_t size, unsigned direction);
int loom_vfio_dma_command(VFIOContainerBase *, unsigned, hwaddr, uint64_t, unsigned);
int loom_vfio_dma_sync(VFIOContainerBase *, bool);
uint64_t loom_vfio_dma_count(VFIOContainerBase *);
#endif
