/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VFIO_LOOM_IAVF_H
#define HW_VFIO_LOOM_IAVF_H
#include "exec/hwaddr.h"
typedef struct VFIODevice VFIODevice;
typedef struct VFIOContainerBase VFIOContainerBase;
typedef struct LoomIAVFQueues LoomIAVFQueues;
/* Control hooks observe already-published DMA baselines. */
/* Caller first publishes completions: 1 scoped, 0 full fallback, <0 error. */
int loom_iavf_submit_rx(VFIODevice *, hwaddr, uint32_t);
void loom_iavf_observe_write(VFIODevice *, hwaddr, uint32_t);
void loom_iavf_observe_completion(VFIODevice *);
void loom_iavf_reset(VFIODevice *);
void loom_iavf_free(VFIODevice *);
/* size == 0 invalidates all state before container destruction. */
void loom_iavf_dma_invalidate_container(VFIOContainerBase *, hwaddr, uint64_t);
#endif
