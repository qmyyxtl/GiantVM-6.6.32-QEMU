/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Observe native X710 VF AdminQ queue configuration. Read only the baseline
 * already published by loom-dma, never resample live hardware DMA. This stage
 * uses acknowledged queue state for scoped RX/TX submissions, with full fallback.
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
    uint16_t vsi, enabled_rx, enabled_tx, pending_rx, pending_tx;
    bool enable_pending;
    uint16_t rx_tail[VF_QUEUE_COUNT], rx_tail_known;
    uint8_t submit_desc[4096 * 32];
    uint64_t submit_payload[4096];
    uint16_t tx_tail[VF_QUEUE_COUNT], tx_tail_known;
    uint32_t tx_length[4096];
};

static void queues_invalidate(LoomIAVFQueues *s)
{
    s->pending = false;
    s->pending_mask = 0;
    s->valid_mask = 0;
    s->enabled_rx = s->enabled_tx = 0;
    s->pending_rx = s->pending_tx = 0;
    s->enable_pending = false;
    s->rx_tail_known = s->tx_tail_known = 0;
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

    if (!dev->loom_iavf_irq_control ||
        !dev->bcontainer || !dev->bcontainer->loom_dma) {
        return NULL;
    }
    if (!dev->loom_iavf_queues) {
        dev->loom_iavf_queues = g_new0(LoomIAVFQueues, 1);
    }
    s = dev->loom_iavf_queues;
    s->epoch = loom_vfio_dma_epoch(dev->bcontainer);
    return s;
}

static bool range_intersects(hwaddr iova, uint64_t size,
                             hwaddr base, uint64_t len)
{
    return len && (iova >= base ? iova - base < len : base - iova < size);
}

void loom_iavf_dma_invalidate_container(VFIOContainerBase *bc,
                                       hwaddr iova, uint64_t size)
{
    VFIODevice *dev;

    QLIST_FOREACH(dev, &bc->device_list, container_next) {
        LoomIAVFQueues *s = dev->loom_iavf_queues;

        if (!s) {
            continue;
        }
        if (!size ||
            range_intersects(iova, size, s->asq_base,
                             s->asq_count * AQ_DESC_SIZE) ||
            range_intersects(iova, size, s->arq_base,
                             s->arq_count * AQ_DESC_SIZE)) {
            loom_iavf_reset(dev);
            continue;
        }
        for (unsigned i = 0; i < VF_QUEUE_COUNT; i++) {
            const LoomIAVFQueue *q = s->pending ? &s->candidate[i] :
                                                &s->queue[i];
            unsigned mask = s->pending ? s->pending_mask : s->valid_mask;

            if ((mask & (1U << i)) &&
                (range_intersects(iova, size, q->tx_iova, q->tx_count * 16ULL) ||
                 range_intersects(iova, size, q->rx_iova, q->rx_count * 32ULL))) {
                /* Do not associate an old ACK with a later candidate. */
                s->ambiguous |= s->pending || s->enable_pending;
                queues_invalidate(s);
                break;
            }
        }
    }
}

static bool queues_covered(VFIODevice *dev, LoomIAVFQueues *s)
{
    for (unsigned i = 0; i < VF_QUEUE_COUNT; i++) {
        LoomIAVFQueue *q = &s->candidate[i];

        if ((s->pending_mask & (1U << i)) &&
            (!loom_vfio_dma_range_covered(dev->bcontainer, q->tx_iova,
                                          q->tx_count * 16ULL) ||
             !loom_vfio_dma_range_covered(dev->bcontainer, q->rx_iova,
                                          q->rx_count * 32ULL))) {
            return false;
        }
    }
    return true;
}

/* virtchnl v1 also retains a legacy extra queue-pair-sized trailing slot. */
static bool queues_parse(LoomIAVFQueues *s, const uint8_t *msg, size_t len)
{
    unsigned pairs;
    uint16_t mask = 0, vsi;

    if (len < 8) {
        return false;
    }
    vsi = lduw_le_p(msg);
    pairs = lduw_le_p(msg + 2);
    if (!pairs || pairs > VF_QUEUE_COUNT ||
        (len != 8 + pairs * 64 && len != 72 + pairs * 64)) {
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
    s->vsi = vsi;
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
    if (op == 8) {
        uint8_t msg[12];
        uint64_t addr = (uint64_t)(uint32_t)ldl_le_p(desc + 24) << 32 |
                        (uint32_t)ldl_le_p(desc + 28);
        uint32_t rx, tx;

        if (s->pending || s->enable_pending || s->ambiguous) {
            queues_invalidate(s);
            s->ambiguous = true;
            return;
        }
        if (!s->valid_mask || lduw_le_p(desc + 4) != sizeof(msg) ||
            !(lduw_le_p(desc) & 0x1000) ||
            loom_vfio_dma_read_baseline(dev->bcontainer, addr, msg, sizeof(msg)) ||
            lduw_le_p(msg) != s->vsi || lduw_le_p(msg + 2)) {
            queues_invalidate(s);
            return;
        }
        rx = (uint32_t)ldl_le_p(msg + 4);
        tx = (uint32_t)ldl_le_p(msg + 8);
        if (!(rx | tx) || ((rx | tx) & ~s->valid_mask)) {
            queues_invalidate(s);
            return;
        }
        s->pending_rx = rx;
        s->pending_tx = tx;
        s->enable_pending = true;
    }
    if (op == 6) {
        unsigned len = lduw_le_p(desc + 4);
        uint64_t addr = (uint64_t)(uint32_t)ldl_le_p(desc + 24) << 32 |
                        (uint32_t)ldl_le_p(desc + 28);
        uint8_t msg[72 + VF_QUEUE_COUNT * 64];

        if (s->pending || s->enable_pending || s->ambiguous) {
            queues_invalidate(s);
            s->ambiguous = true;
            return;
        }
        queues_invalidate(s);
        if (!(lduw_le_p(desc) & 0x1000) || len > sizeof(msg) ||
            loom_vfio_dma_read_baseline(dev->bcontainer, addr, msg, len) ||
            !queues_parse(s, msg, len) || !queues_covered(dev, s)) {
            return;
        }
        s->pending = true;
    }
}

void loom_iavf_observe_write(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    LoomIAVFQueues *s;

    if (addr <= 0x3c && !(addr & 3)) {
        unsigned id = addr / 4;
        s = dev->loom_iavf_queues;
        if (s && (s->valid_mask & s->enabled_tx & (1U << id))) {
            if (value < s->queue[id].tx_count) {
                s->tx_tail[id] = value;
                s->tx_tail_known |= 1U << id;
            } else {
                s->tx_tail_known &= ~(1U << id);
            }
        }
        return;
    }
    if (addr >= 0x2000 && addr <= 0x203c && !(addr & 3)) {
        unsigned id = (addr - 0x2000) / 4;
        s = dev->loom_iavf_queues;
        if (s && (s->valid_mask & s->enabled_rx & (1U << id))) {
            if (value < s->queue[id].rx_count) {
                s->rx_tail[id] = value;
                s->rx_tail_known |= 1U << id;
            } else {
                s->rx_tail_known &= ~(1U << id);
            }
        }
        return;
    }
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
                !ldl_le_p(desc + 12) && queues_covered(dev, s)) {
                memcpy(s->queue, s->candidate, sizeof(s->queue));
                s->valid_mask = s->pending_mask;
                error_report("Loom iavf %s queue config observed mask=0x%x",
                             dev->name, s->valid_mask);
            } else {
                s->valid_mask = 0;
            }
            s->pending = false;
            s->pending_mask = 0;
        } else if (op == 8) {
            if (s->enable_pending && !s->ambiguous && s->valid_mask &&
                !(lduw_le_p(desc) & 4) && !lduw_le_p(desc + 6) &&
                !ldl_le_p(desc + 12)) {
                s->enabled_rx |= s->pending_rx;
                s->enabled_tx |= s->pending_tx;
            } else {
                queues_invalidate(s);
            }
            s->enable_pending = false;
            s->pending_rx = s->pending_tx = 0;
        } else if (op == 17) {
            uint8_t event[4];
            uint64_t addr = (uint64_t)(uint32_t)ldl_le_p(desc + 24) << 32 |
                            (uint32_t)ldl_le_p(desc + 28);

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


/*
 * Native iavf replenishes [old next_to_use, new next_to_use), and clears the
 * next descriptor's status qword. Stage all payload coverage before publishing
 * any bytes; preserve RX page reuse by merging payloads as well as descriptors.
 * The caller has already published device completions, including late DMA.
 */
int loom_iavf_submit_rx(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    static int scoped = -1;
    LoomIAVFQueues *s;
    LoomIAVFQueue *q;
    unsigned id, count, old, first;
    uint8_t *desc;
    uint64_t *payload;
    int ret;

    if (addr < 0x2000 || addr > 0x203c || (addr & 3) ||
        !dev->loom_iavf_irq_control) {
        return 0;
    }
    if (scoped < 0) {
        const char *mode = getenv("LOOM_IAVF_RX_SCOPED");
        scoped = !mode || strcmp(mode, "0");
    }
    if (!scoped) {
        return 0;
    }
    s = dev->loom_iavf_queues;
    id = (addr - 0x2000) / 4;
    if (!s || !(s->valid_mask & s->enabled_rx & s->rx_tail_known &
                (1U << id)) || s->pending || s->enable_pending || s->ambiguous) {
        return 0;
    }
    q = &s->queue[id];
    desc = s->submit_desc;
    payload = s->submit_payload;
    old = s->rx_tail[id];
    if (value >= q->rx_count || old >= q->rx_count || value == old) {
        return 0;
    }
    count = (value + q->rx_count - old) % q->rx_count;
    first = MIN(count, q->rx_count - old);
    ret = loom_vfio_dma_read_cpu(dev->bcontainer, q->rx_iova + old * 32ULL,
                                 desc, first * 32ULL);
    if (!ret && first < count) {
        ret = loom_vfio_dma_read_cpu(dev->bcontainer, q->rx_iova,
                                     desc + first * 32, (count - first) * 32ULL);
    }
    if (ret) {
        return ret == -ENOENT ? 0 : ret;
    }
    for (unsigned i = 0; i < count; i++) {
        /* Header split, non-native layouts, and unmapped buffers fall back. */
        payload[i] = ldq_le_p(desc + i * 32);
        if (ldq_le_p(desc + i * 32 + 8) || !payload[i] ||
            !loom_vfio_dma_range_covered(dev->bcontainer, payload[i],
                                          q->rx_buffer_size)) {
            return 0;
        }
    }
    if (!loom_vfio_dma_range_covered(dev->bcontainer, q->rx_iova,
                                      q->rx_count * 32ULL)) {
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer, payload[i],
                                                 q->rx_buffer_size);
        if (ret) {
            return ret;
        }
    }
    /* Publish payloads first, then descriptors, before the physical tail. */
    ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer,
                            q->rx_iova + old * 32ULL, first * 32ULL);
    if (!ret && first < count) {
        ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer, q->rx_iova,
                                                 (count - first) * 32ULL);
    }
    if (!ret) {
        ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer,
                                         q->rx_iova + value * 32ULL + 8, 8);
    }
    return ret ? ret : 1;
}


/*
 * Native iavf TX tails cover [old next_to_use,new next_to_use). Data descriptors
 * contain a 64-bit IOVA and a 14-bit byte count; context descriptors carry no
 * payload address. Preflight every data fragment, including mapped-page splits,
 * then publish its bytes before the corresponding descriptor batch.
 */
int loom_iavf_submit_tx(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    static int scoped = -1;
    LoomIAVFQueues *s;
    LoomIAVFQueue *q;
    unsigned id, count, old, first;
    uint8_t *desc;
    uint64_t *payload;
    uint32_t *length;
    int ret;

    if (addr > 0x3c || (addr & 3) || !dev->loom_iavf_irq_control) {
        return 0;
    }
    if (scoped < 0) {
        const char *mode = getenv("LOOM_IAVF_TX_SCOPED");
        scoped = !mode || strcmp(mode, "0");
    }
    if (!scoped) {
        return 0;
    }
    s = dev->loom_iavf_queues;
    id = addr / 4;
    if (!s || !(s->valid_mask & s->enabled_tx & s->tx_tail_known &
                (1U << id)) || s->pending || s->enable_pending || s->ambiguous) {
        return 0;
    }
    q = &s->queue[id];
    desc = s->submit_desc;
    payload = s->submit_payload;
    length = s->tx_length;
    old = s->tx_tail[id];
    if (value >= q->tx_count || old >= q->tx_count || value == old) {
        return 0;
    }
    count = (value + q->tx_count - old) % q->tx_count;
    first = MIN(count, q->tx_count - old);
    ret = loom_vfio_dma_read_cpu(dev->bcontainer, q->tx_iova + old * 16ULL,
                                 desc, first * 16ULL);
    if (!ret && first < count) {
        ret = loom_vfio_dma_read_cpu(dev->bcontainer, q->tx_iova,
                                     desc + first * 16, (count - first) * 16ULL);
    }
    if (ret) {
        return ret == -ENOENT ? 0 : ret;
    }
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *d = desc + i * 16;
        uint64_t command = ldq_le_p(d + 8);
        unsigned type = command & 15;

        length[i] = 0;
        if (type == 1) {
            /* Native TSO/VLAN/tunnel context has no external DMA address. */
            if (lduw_le_p(d + 6) ||
                (((command >> 4) & 0xffff) & ~0x7fU) ||
                (command & ((UINT64_C(0x3ff) << 20) |
                            (UINT64_C(3) << 48)))) {
                return 0;
            }
            continue;
        }
        if (type != 0) {
            return 0;
        }
        payload[i] = ldq_le_p(d);
        length[i] = (command >> 34) & 0x3fff;
        if (!payload[i] || !length[i] ||
            !loom_vfio_dma_range_covered(dev->bcontainer, payload[i], length[i])) {
            return 0;
        }
    }
    if (!loom_vfio_dma_range_covered(dev->bcontainer, q->tx_iova,
                                     q->tx_count * 16ULL)) {
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        if (!length[i]) {
            continue;
        }
        ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer,
                                                 payload[i], length[i]);
        if (ret) {
            return ret;
        }
    }
    ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer,
                              q->tx_iova + old * 16ULL, first * 16ULL);
    if (!ret && first < count) {
        ret = loom_vfio_dma_sync_to_device_range(dev->bcontainer, q->tx_iova,
                                                 (count - first) * 16ULL);
    }
    return ret ? ret : 1;
}
