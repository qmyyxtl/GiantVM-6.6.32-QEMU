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
int loom_iavf_submit_tx(VFIODevice *, hwaddr, uint32_t);
void loom_iavf_observe_write(VFIODevice *, hwaddr, uint32_t);
void loom_iavf_observe_completion(VFIODevice *);
/* Reconcile only completions already published by the full DMA path. */
void loom_iavf_reconcile_completion(VFIODevice *);
/* 1 scoped data completion, 0 full fallback, negative synchronization error. */
int loom_iavf_sync_data_completion(VFIODevice *);
void loom_iavf_reset(VFIODevice *);
void loom_iavf_free(VFIODevice *);
/* size == 0 invalidates all state before container destruction. */
void loom_iavf_dma_invalidate_container(VFIOContainerBase *, hwaddr, uint64_t);
/*
 * Physical AdminQ gate callback: zero prepared, negative error; no BAR write.
 * Caller publishes prior completions, then authorizes/maps/synchronizes all
 * submitted dependencies before hardware enable/tail. Requires sparse RAM.
 */
int loom_iavf_prepare_physical_admin(VFIODevice *, bool arq, bool enabling,
                                    uint64_t base, unsigned count,
                                    unsigned next, unsigned tail);
#endif
