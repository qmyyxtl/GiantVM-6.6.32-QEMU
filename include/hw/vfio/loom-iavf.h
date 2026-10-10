/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_VFIO_LOOM_IAVF_H
#define HW_VFIO_LOOM_IAVF_H
#include "exec/hwaddr.h"
typedef struct VFIODevice VFIODevice;
typedef struct LoomIAVFQueues LoomIAVFQueues;
/* Observer only: these hooks never replace DMA synchronization. */
void loom_iavf_observe_write(VFIODevice *, hwaddr, uint32_t);
void loom_iavf_observe_completion(VFIODevice *);
void loom_iavf_reset(VFIODevice *);
void loom_iavf_free(VFIODevice *);
#endif
