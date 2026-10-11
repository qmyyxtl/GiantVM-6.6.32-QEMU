/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VFIO_LOOM_IAVF_H
#define HW_VFIO_LOOM_IAVF_H
#include "exec/hwaddr.h"
typedef struct VFIODevice VFIODevice;
typedef struct VFIORegion VFIORegion;
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
/* Optional native AdminQ + data completion; zero means untouched full fallback. */
int loom_iavf_sync_control_completion(VFIODevice *);
/* Optional completion before known native control writes; submission unchanged. */
int loom_iavf_sync_control_write(VFIORegion *, hwaddr, unsigned);
void loom_iavf_reset(VFIODevice *);
void loom_iavf_free(VFIODevice *);
/* size == 0 invalidates all state before container destruction. */
void loom_iavf_dma_invalidate_container(VFIOContainerBase *, hwaddr, uint64_t);
/*
 * Physical AdminQ gate callback: zero prepared, negative error; no BAR write.
 * Caller publishes prior completions, then authorizes/maps/synchronizes all
 * submitted dependencies before hardware enable/tail. Requires sparse RAM.
 * Unposted all-zero RX slots are ignored; caller must validate posted data tails.
 */
int loom_iavf_prepare_physical_admin(VFIODevice *, bool arq, bool enabling,
                                    uint64_t base, unsigned count,
                                    unsigned next, unsigned tail);
/*
 * Sparse data preparation, no BAR writes:
 * 0 not a physical data doorbell; 1 ready; 2 deferred (MUST NOT forward); <0 error.
 * Caller publishes/reconciles prior completions, then prepares, observes the
 * original write with record_physical_data (which retains the old tail), writes
 * the hardware tail, and commits only after success.
 * A write/prepare failure is fatal to the mode; keep pools until DMA stops.
 * Before ENABLE_QUEUES, flush deferred RX tails only after config ACK and
 * validate/prepare every newly exposed slot. Do not infer a ring from a tail.
 */
int loom_iavf_prepare_physical_data(VFIODevice *, hwaddr, uint32_t);
int loom_iavf_record_physical_data(VFIODevice *, hwaddr, uint32_t);
int loom_iavf_commit_physical_data(VFIODevice *, hwaddr, uint32_t);
bool loom_iavf_deferred_physical_rx(VFIODevice *, unsigned, uint32_t *);
/* 1 config ACK ready for RX hints, 0 not ready, negative invalid state. */
int loom_iavf_physical_rx_configuration(VFIODevice *);
/* Adapter: 1 handled (including deferred), 0 legacy, negative fatal. */
int loom_iavf_physical_bar_write(VFIORegion *, hwaddr, uint64_t, unsigned);
void loom_iavf_physical_bar_read(VFIORegion *, hwaddr, unsigned, uint64_t *);
#endif
