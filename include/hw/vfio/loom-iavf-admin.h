/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VFIO_LOOM_IAVF_ADMIN_H
#define HW_VFIO_LOOM_IAVF_ADMIN_H
#include "exec/hwaddr.h"

typedef struct LoomIAVFAdminQueue {
    uint64_t base;
    uint32_t length;
    uint16_t next;
    uint8_t configured;
    bool hardware_enabled;
} LoomIAVFAdminQueue;

typedef struct LoomIAVFAdminState {
    LoomIAVFAdminQueue queue[2]; /* ASQ, ARQ */
    bool failed;
} LoomIAVFAdminState;

/* All callbacks return zero on success or a negative errno on failure. */
typedef struct LoomIAVFAdminOps {
    int (*write)(void *, hwaddr, uint32_t);
    int (*read)(void *, hwaddr, uint32_t *);
    /*
     * With enabling=true, materialize and synchronize the whole ring, plus
     * every ARQ receive buffer, before hardware enable. For a tail, discover,
     * authorize, materialize and synchronize all newly submitted dependencies:
     * ASQ [next,tail), ARQ [next,tail] modulo count. ASQ commands can describe
     * further data-queue rings/buffers; unsupported commands must return error.
     * Caller publishes prior device completions before entering this gate.
     */
    int (*prepare)(void *, bool arq, bool enabling, uint64_t base,
                   unsigned count, unsigned next, unsigned tail);
} LoomIAVFAdminOps;

/*
 * Caller holds BQL. Zero-initialize only after hardware reset/quiescence.
 * 1 handled, 0 not an AdminQ register, negative fatal error: never resume the
 * mode or free DMA pools until hardware access is stopped/its backend closes.
 * BAR adapter is connected only for sparse RAM; no startup flag exists yet.
 */
int loom_iavf_admin_write(LoomIAVFAdminState *, hwaddr, uint32_t,
                          const LoomIAVFAdminOps *, void *);
/*
 * While configuration is pending, merge requested count/enable with hardware
 * status. Running/failed queues expose actual hardware count/enable unchanged.
 */
bool loom_iavf_admin_read(const LoomIAVFAdminState *, hwaddr, uint32_t *);
#endif
