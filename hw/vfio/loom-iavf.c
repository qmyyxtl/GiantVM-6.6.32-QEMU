/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Observe native X710 VF AdminQ queue configuration. Read only the baseline
 * already published by loom-dma, never resample live hardware DMA. This stage
 * does not enable queue-scoped submission or change the full-sync fallback.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "hw/vfio/vfio-common.h"
#include "hw/vfio/loom-dma.h"
#include "hw/vfio/loom-iavf.h"

#define AQ_DESC_SIZE 32
#define AQ_MAX_COUNT 1024
#define VF_QUEUE_COUNT 16

typedef struct LoomIAVFQueue {
    uint64_t rx_iova, tx_iova;
    uint32_t rx_count, tx_count, rx_buffer_size;
} LoomIAVFQueue;

struct LoomIAVFQueues {
    uint64_t asq_base, arq_base, epoch;
    uint16_t asq_count, arq_count, asq_next, arq_next;
    bool asq_enabled, arq_enabled, pending, ambiguous;
    uint16_t pending_mask, valid_mask;
    LoomIAVFQueue candidate[VF_QUEUE_COUNT], queue[VF_QUEUE_COUNT];
};

static void queues_invalidate(LoomIAVFQueues *s)
{
    s->pending = false;
    s->pending_mask = 0;
    s->valid_mask = 0;
}

void loom_iavf_reset(VFIODevice *dev)
{
    if (dev->loom_iavf_queues) {
        memset(dev->loom_iavf_queues, 0, sizeof(LoomIAVFQueues));
    }
}

void loom_iavf_free(VFIODevice *dev)
{
    g_free(dev->loom_iavf_queues);
    dev->loom_iavf_queues = NULL;
}

static LoomIAVFQueues *queues_get(VFIODevice *dev)
{
    LoomIAVFQueues *s;
    uint64_t epoch;

    if (!dev->loom_iavf_irq_control ||
        !dev->bcontainer || !dev->bcontainer->loom_dma) {
        return NULL;
    }
    if (!dev->loom_iavf_queues) {
        dev->loom_iavf_queues = g_new0(LoomIAVFQueues, 1);
    }
    s = dev->loom_iavf_queues;
    epoch = loom_vfio_dma_epoch(dev->bcontainer);
    if (s->epoch != epoch) {
        queues_invalidate(s);
        s->epoch = epoch;
    }
    return s;
}

/* virtchnl v1: 8-byte header, followed by 64-byte TX/RX queue pairs. */
static bool queues_parse(LoomIAVFQueues *s, const uint8_t *msg, size_t len)
{
    unsigned pairs;
    uint16_t mask = 0, vsi;

    if (len < 8) {
        return false;
    }
    vsi = lduw_le_p(msg);
    pairs = lduw_le_p(msg + 2);
    if (!pairs || pairs > VF_QUEUE_COUNT || len != 8 + pairs * 64) {
        return false;
    }
    for (unsigned i = 0; i < pairs; i++) {
        const uint8_t *tx = msg + 8 + i * 64;
        const uint8_t *rx = tx + 24;
        unsigned id = lduw_le_p(tx + 2);
        LoomIAVFQueue q = {
            .tx_count = lduw_le_p(tx + 4),
            .tx_iova = ldq_le_p(tx + 8),
            .rx_count = ldl_le_p(rx + 4),
            .rx_buffer_size = ldl_le_p(rx + 12),
            .rx_iova = ldq_le_p(rx + 24),
        };

        if (id >= VF_QUEUE_COUNT || mask & (1U << id) ||
            lduw_le_p(tx) != vsi || lduw_le_p(rx) != vsi ||
            lduw_le_p(rx + 2) != id ||
            lduw_le_p(tx + 6) || ldq_le_p(tx + 16) ||
            lduw_le_p(rx + 8) || lduw_le_p(rx + 10) || rx[21] ||
            !q.tx_count || q.tx_count > 4096 || q.tx_count % 8 ||
            !q.rx_count || q.rx_count > 4096 || q.rx_count % 32 ||
            !q.tx_iova || q.tx_iova % 128 ||
            !q.rx_iova || q.rx_iova % 128 ||
            q.tx_iova > UINT64_MAX - q.tx_count * 16ULL ||
            q.rx_iova > UINT64_MAX - q.rx_count * 32ULL ||
            !q.rx_buffer_size || q.rx_buffer_size > 65536) {
            return false;
        }
        s->candidate[id] = q;
        mask |= 1U << id;
    }
    s->pending_mask = mask;
    return true;
}

static void observe_request(VFIODevice *dev, LoomIAVFQueues *s,
                             const uint8_t *desc)
{
    unsigned op = ldl_le_p(desc + 8);

    if (lduw_le_p(desc + 2) != 0x0801) {
        queues_invalidate(s);
        return;
    }
    if (op == 2 || op == 4 || op == 5 || op == 9 || op == 29) {
        queues_invalidate(s);
    }
    if (op == 2) {
        s->ambiguous = false;
    }
    if (op == 6) {
        unsigned len = lduw_le_p(desc + 4);
        uint64_t addr = (uint64_t)ldl_le_p(desc + 24) << 32 |
                        ldl_le_p(desc + 28);
        uint8_t msg[8 + VF_QUEUE_COUNT * 64];

        if (s->pending || s->ambiguous) {
            queues_invalidate(s);
            s->ambiguous = true;
            return;
        }
        queues_invalidate(s);
        if (!(lduw_le_p(desc) & 0x1000) || len > sizeof(msg) ||
            loom_vfio_dma_read_baseline(dev->bcontainer, addr, msg, len) ||
            !queues_parse(s, msg, len)) {
            return;
        }
        s->pending = true;
    }
}

void loom_iavf_observe_write(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    LoomIAVFQueues *s;

    if (addr != 0x7c00 && addr != 0x7800 && addr != 0x6800 &&
        addr != 0x6c00 && addr != 0x6000 && addr != 0x8000 &&
        addr != 0x8400) {
        return;
    }
    s = queues_get(dev);
    if (!s) {
        return;
    }
    switch (addr) {
    case 0x7c00:
        queues_invalidate(s);
        s->asq_base = (s->asq_base & ~UINT64_C(0xffffffff)) | value;
        s->asq_next = 0;
        s->ambiguous = false;
        break;
    case 0x7800:
        queues_invalidate(s);
        s->asq_base = (s->asq_base & UINT32_MAX) | (uint64_t)value << 32;
        s->asq_next = 0;
        s->ambiguous = false;
        break;
    case 0x6800:
        queues_invalidate(s);
        s->asq_count = value & 0x3ff;
        s->asq_enabled = value & (1U << 31);
        s->asq_next = 0;
        s->ambiguous = false;
        break;
    case 0x6c00:
        queues_invalidate(s);
        s->arq_base = (s->arq_base & ~UINT64_C(0xffffffff)) | value;
        s->arq_next = 0;
        break;
    case 0x6000:
        queues_invalidate(s);
        s->arq_base = (s->arq_base & UINT32_MAX) | (uint64_t)value << 32;
        s->arq_next = 0;
        break;
    case 0x8000:
        queues_invalidate(s);
        s->arq_count = value & 0x3ff;
        s->arq_enabled = value & (1U << 31);
        s->arq_next = 0;
        break;
    case 0x8400:
        if (!s->asq_enabled || !s->asq_base ||
            !s->asq_count || s->asq_count >= AQ_MAX_COUNT ||
            value >= s->asq_count || s->asq_next >= s->asq_count ||
            s->asq_base > UINT64_MAX - s->asq_count * AQ_DESC_SIZE) {
            queues_invalidate(s);
            return;
        }
        while (s->asq_next != value) {
            uint8_t desc[AQ_DESC_SIZE];

            if (loom_vfio_dma_read_baseline(dev->bcontainer,
                    s->asq_base + s->asq_next * AQ_DESC_SIZE,
                    desc, sizeof(desc))) {
                queues_invalidate(s);
                s->asq_next = value;
                return;
            }
            observe_request(dev, s, desc);
            s->asq_next = (s->asq_next + 1) % s->asq_count;
        }
        break;
    }
}

void loom_iavf_observe_completion(VFIODevice *dev)
{
    LoomIAVFQueues *s = queues_get(dev);
    unsigned count;

    if (!s || !s->arq_enabled || !s->arq_base ||
        !s->arq_count || s->arq_count >= AQ_MAX_COUNT ||
        s->arq_base > UINT64_MAX - s->arq_count * AQ_DESC_SIZE) {
        return;
    }
    for (count = 0; count < s->arq_count; count++) {
        uint8_t desc[AQ_DESC_SIZE];
        unsigned op;

        if (loom_vfio_dma_read_baseline(dev->bcontainer,
                s->arq_base + s->arq_next * AQ_DESC_SIZE,
                desc, sizeof(desc))) {
            queues_invalidate(s);
            return;
        }
        if (!(lduw_le_p(desc) & 1)) {
            break;
        }
        op = ldl_le_p(desc + 8);
        if (lduw_le_p(desc + 2) != 0x0801 &&
            lduw_le_p(desc + 2) != 0x0802) {
            queues_invalidate(s);
        } else if (op == 6) {
            if (s->pending && !s->ambiguous &&
                !(lduw_le_p(desc) & 4) && !lduw_le_p(desc + 6) &&
                !ldl_le_p(desc + 12)) {
                memcpy(s->queue, s->candidate, sizeof(s->queue));
                s->valid_mask = s->pending_mask;
                error_report("Loom iavf %s queue config observed mask=0x%x",
                             dev->name, s->valid_mask);
            } else {
                s->valid_mask = 0;
            }
            s->pending = false;
            s->pending_mask = 0;
        } else if (op == 17) {
            uint8_t event[4];
            uint64_t addr = (uint64_t)ldl_le_p(desc + 24) << 32 |
                            ldl_le_p(desc + 28);

            if (lduw_le_p(desc + 4) < sizeof(event) ||
                loom_vfio_dma_read_baseline(dev->bcontainer, addr,
                                            event, sizeof(event)) ||
                ldl_le_p(event) == 2 || ldl_le_p(event) == 3) {
                queues_invalidate(s);
            }
        }
        s->arq_next = (s->arq_next + 1) % s->arq_count;
    }
    if (count == s->arq_count) {
        queues_invalidate(s);
        s->ambiguous = true;
    }
}
