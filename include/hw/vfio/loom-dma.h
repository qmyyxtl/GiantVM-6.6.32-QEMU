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
/* will_initialize requires full pool initialization before hardware mapping. */
int loom_vfio_dma_map(VFIOContainerBase *, hwaddr, uint64_t, void **,
                      bool readonly, bool will_initialize);
int loom_vfio_dma_unmap(VFIOContainerBase *, hwaddr, uint64_t);
void loom_vfio_dma_free(VFIOContainerBase *);
int loom_vfio_dma_register(VFIOContainerBase *bc, hwaddr iova, hwaddr gpa,
                           uint64_t size, unsigned direction);
int loom_vfio_dma_command(VFIOContainerBase *, unsigned, hwaddr, uint64_t, unsigned);
int loom_vfio_dma_sync(VFIOContainerBase *, bool);
/* CPU submission only; caller preserves completion and doorbell ordering. */
int loom_vfio_dma_sync_to_device_range(VFIOContainerBase *, hwaddr, uint64_t);
bool loom_vfio_dma_range_covered(VFIOContainerBase *, hwaddr, uint64_t);
uint64_t loom_vfio_dma_count(VFIOContainerBase *);
uint64_t loom_vfio_dma_epoch(VFIOContainerBase *);
int loom_vfio_dma_read_cpu(VFIOContainerBase *, hwaddr, void *, uint64_t);
int loom_vfio_dma_read_baseline(VFIOContainerBase *, hwaddr, void *, uint64_t);
/* Device snapshot capture and exact publication; caller orders dependencies. */
int loom_vfio_dma_read_pool(VFIOContainerBase *, hwaddr, void *, uint64_t);
int loom_vfio_dma_publish_snapshot(VFIOContainerBase *, hwaddr,
                                   const void *, uint64_t);
/* Registration lifetime of a native device-writable page, or zero. */
uint64_t loom_vfio_dma_page_generation(VFIOContainerBase *, hwaddr);
/*
 * Physical DMA prototype: caller holds BQL and supplies system-memory RAM
 * sections only. Authorization installs no hardware map. Materialization must
 * finish before any descriptor/doorbell exposes these bytes to the device.
 * Remove requires the caller to stop DMA and never resume on a failed revoke.
 */
int loom_vfio_ram_add(VFIOContainerBase *, hwaddr, uint64_t, bool readonly);
int loom_vfio_ram_remove(VFIOContainerBase *, hwaddr, uint64_t);
/* Queue discovery only: authorized CPU bytes, no pool allocation or DMA map. */
int loom_vfio_ram_read_cpu(VFIOContainerBase *, hwaddr, void *, uint64_t);
int loom_vfio_ram_materialize(VFIOContainerBase *, hwaddr, uint64_t,
                              bool device_writes);
#endif
