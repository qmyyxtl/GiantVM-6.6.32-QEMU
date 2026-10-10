/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Observe native X710 VF AdminQ queue configuration. Control observation reads published baselines.
 * Acknowledged native data queues use scoped submission and immutable DMA
 * completion snapshots, with full synchronization for unsupported state.
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

typedef struct LoomIAVFRXSlot {
    uint64_t iova, first_generation, last_generation;
} LoomIAVFRXSlot;

typedef struct LoomIAVFCompletion {
    LoomIAVFRXSlot *rx_slot;
    uint8_t *rx_snapshot, *tx_snapshot;
    unsigned rx_next;
    bool seeded;
} LoomIAVFCompletion;

typedef struct LoomIAVFPhysicalData {
    uint16_t deferred_rx[VF_QUEUE_COUNT];
    uint16_t prepared_rx[VF_QUEUE_COUNT], prepared_tx[VF_QUEUE_COUNT];
    uint16_t deferred_mask, rx_prepared_mask, tx_prepared_mask;
} LoomIAVFPhysicalData;

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
    LoomIAVFCompletion completion[VF_QUEUE_COUNT];
    uint8_t *completion_payload;
    LoomIAVFPhysicalData physical;
};

static void completion_free(LoomIAVFQueues *s)
{
    g_clear_pointer(&s->completion_payload, g_free);
    for (unsigned i = 0; i < VF_QUEUE_COUNT; i++) {
        LoomIAVFCompletion *c = &s->completion[i];

        g_free(c->rx_slot);
        g_free(c->rx_snapshot);
        g_free(c->tx_snapshot);
        memset(c, 0, sizeof(*c));
    }
}

static void completion_init(LoomIAVFQueues *s)
{
    completion_free(s);
    s->completion_payload = g_malloc(4096);
    for (unsigned i = 0; i < VF_QUEUE_COUNT; i++) {
        LoomIAVFQueue *q = &s->queue[i];
        LoomIAVFCompletion *c = &s->completion[i];

        if (!(s->valid_mask & (1U << i))) {
            continue;
        }
        c->rx_slot = g_new0(LoomIAVFRXSlot, q->rx_count);
        c->rx_snapshot = g_malloc(q->rx_count * 32ULL);
        c->tx_snapshot = g_malloc(q->tx_count * 16ULL);
    }
}

static bool rx_slot_set(VFIODevice *dev, const LoomIAVFQueue *q,
                         LoomIAVFRXSlot *slot, uint64_t iova)
{
    *slot = (LoomIAVFRXSlot) { .iova = iova };
    if (!iova || !q->rx_buffer_size || q->rx_buffer_size > 4096 ||
        iova > UINT64_MAX - q->rx_buffer_size) {
        return false;
    }
    slot->first_generation = loom_vfio_dma_page_generation(dev->bcontainer,
                                                          iova);
    slot->last_generation = loom_vfio_dma_page_generation(dev->bcontainer,
                                         iova + q->rx_buffer_size - 1);
    return slot->first_generation && slot->last_generation;
}

static bool rx_slot_valid(VFIODevice *dev, const LoomIAVFQueue *q,
                           const LoomIAVFRXSlot *slot)
{
    return slot->iova && slot->first_generation &&
        slot->first_generation == loom_vfio_dma_page_generation(dev->bcontainer,
                                                                 slot->iova) &&
        slot->last_generation == loom_vfio_dma_page_generation(dev->bcontainer,
                                  slot->iova + q->rx_buffer_size - 1);
}

/* Capture addresses before ENABLE_QUEUES can let hardware erase them. */
static void completion_seed_rx(VFIODevice *dev, LoomIAVFQueues *s, unsigned id)
{
    LoomIAVFQueue *q = &s->queue[id];
    LoomIAVFCompletion *c = &s->completion[id];

    c->seeded = false;
    c->rx_next = 0;
    if (!c->rx_slot || q->rx_buffer_size > 4096) {
        return;
    }
    memset(c->rx_slot, 0, q->rx_count * sizeof(*c->rx_slot));
    for (unsigned n = 0; n < q->rx_count; n++) {
        uint8_t desc[32];
        uint64_t address;

        if (loom_vfio_dma_read_baseline(dev->bcontainer, q->rx_iova + n * 32ULL,
                                        desc, sizeof(desc)) ||
            ldq_le_p(desc + 8)) {
            return;
        }
        address = ldq_le_p(desc);
        if (address && !rx_slot_set(dev, q, &c->rx_slot[n], address)) {
            return;
        }
    }
    c->seeded = true;
}

/* Harvest rearmed slots after CPU submission but before the physical tail. */
static void completion_refresh_rx(VFIODevice *dev, LoomIAVFQueues *s,
                                   unsigned id, uint32_t tail)
{
    LoomIAVFQueue *q = &s->queue[id];
    LoomIAVFCompletion *c = &s->completion[id];
    unsigned start, count;

    if (!c->seeded || tail >= q->rx_count) {
        return;
    }
    if (s->rx_tail_known & (1U << id)) {
        start = s->rx_tail[id];
        count = (tail + q->rx_count - start) % q->rx_count;
    } else {
        start = 0;
        count = q->rx_count;
    }
    for (unsigned n = 0; n < count; n++) {
        unsigned slot = (start + n) % q->rx_count;
        uint8_t desc[32];

        /*
         * The tail is the exclusive next_to_use sentinel. Native iavf clears
         * its status but may retain hardware RSS writeback in qword 0. On the
         * first tail, do not mistake that unposted slot for a DMA address.
         * Subsequent [old_tail, new_tail) ranges already exclude it.
         */
        if (!(s->rx_tail_known & (1U << id)) && slot == tail) {
            continue;
        }
        if (loom_vfio_dma_read_baseline(dev->bcontainer,
                         q->rx_iova + slot * 32ULL, desc, sizeof(desc))) {
            c->seeded = false;
            return;
        }
        /* The first tail may also cover already completed or unused slots. */
        if (ldq_le_p(desc + 8)) {
            if (s->rx_tail_known & (1U << id)) {
                c->seeded = false;
                return;
            }
            continue;
        }
        uint64_t address = ldq_le_p(desc);
        if (!address && !(s->rx_tail_known & (1U << id))) {
            continue;
        }
        if (!rx_slot_set(dev, q, &c->rx_slot[slot], address)) {
            c->seeded = false;
            return;
        }
    }
}

/*
 * Full DMA publication can let the guest consume slots before the next scoped
 * interrupt. Advance the cache from published baselines before a tail's CPU
 * submission clears those completion flags; never resample hardware here.
 */
void loom_iavf_reconcile_completion(VFIODevice *dev)
{
    LoomIAVFQueues *s = dev->loom_iavf_queues;

    if (!s) {
        return;
    }
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        LoomIAVFQueue *q = &s->queue[id];
        LoomIAVFCompletion *c = &s->completion[id];

        if (!(s->valid_mask & s->enabled_rx & (1U << id)) || !c->seeded) {
            continue;
        }
        for (unsigned n = 0; n < q->rx_count; n++) {
            LoomIAVFRXSlot *slot = &c->rx_slot[c->rx_next];
            uint8_t status[8];

            if (!slot->iova) {
                break;
            }
            if (!rx_slot_valid(dev, q, slot) ||
                loom_vfio_dma_read_baseline(dev->bcontainer,
                   q->rx_iova + c->rx_next * 32ULL + 8, status, sizeof(status))) {
                c->seeded = false;
                break;
            }
            if (!(ldq_le_p(status) & 1)) {
                break;
            }
            memset(slot, 0, sizeof(*slot));
            c->rx_next = (c->rx_next + 1) % q->rx_count;
        }
    }
}

static void queues_invalidate(LoomIAVFQueues *s)
{
    completion_free(s);
    s->pending = false;
    s->pending_mask = 0;
    s->valid_mask = 0;
    s->enabled_rx = s->enabled_tx = 0;
    s->pending_rx = s->pending_tx = 0;
    s->enable_pending = false;
    s->rx_tail_known = s->tx_tail_known = 0;
    s->physical.rx_prepared_mask = s->physical.tx_prepared_mask = 0;
    /* Deferred initial RX tails must survive the CONFIG request and its ACK. */
}

void loom_iavf_reset(VFIODevice *dev)
{
    if (dev->loom_iavf_queues) {
        completion_free(dev->loom_iavf_queues);
        memset(dev->loom_iavf_queues, 0, sizeof(LoomIAVFQueues));
    }
}

void loom_iavf_free(VFIODevice *dev)
{
    if (dev->loom_iavf_queues) {
        completion_free(dev->loom_iavf_queues);
    }
    g_free(dev->loom_iavf_queues);
    dev->loom_iavf_queues = NULL;
}

static LoomIAVFQueues *queues_get(VFIODevice *dev)
{
    LoomIAVFQueues *s;

    /* Physical queue bases are programmed before their first active pool. */
    if (!dev->loom_iavf_irq_control || !dev->bcontainer ||
        (!dev->bcontainer->loom_dma && !dev->bcontainer->loom_ram)) {
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
static bool queue_config_parse(LoomIAVFQueue *candidate, uint16_t *out_mask,
                                uint16_t *out_vsi, const uint8_t *msg,
                                size_t len)
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
        candidate[id] = q;
        mask |= 1U << id;
    }
    *out_mask = mask;
    *out_vsi = vsi;
    return true;
}

static bool queues_parse(LoomIAVFQueues *s, const uint8_t *msg, size_t len)
{
    return queue_config_parse(s->candidate, &s->pending_mask, &s->vsi, msg, len);
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
    if (op == 2 || op == 9 || op == 29) {
        s->physical.deferred_mask = 0;
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
        for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
            if (rx & (1U << id)) {
                completion_seed_rx(dev, s, id);
            }
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
                completion_refresh_rx(dev, s, id, value);
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
                completion_init(s);
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

static bool payload_intersects_descriptors(LoomIAVFQueues *s, uint64_t iova,
                                            uint32_t size)
{
    if (range_intersects(iova, size, s->asq_base,
                         s->asq_count * AQ_DESC_SIZE) ||
        range_intersects(iova, size, s->arq_base,
                         s->arq_count * AQ_DESC_SIZE)) {
        return true;
    }
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        LoomIAVFQueue *q = &s->queue[id];

        if ((s->valid_mask & (1U << id)) &&
            (range_intersects(iova, size, q->rx_iova, q->rx_count * 32ULL) ||
             range_intersects(iova, size, q->tx_iova, q->tx_count * 16ULL))) {
            return true;
        }
    }
    return false;
}

static bool tx_snapshot_types_valid(const uint8_t *snapshot, unsigned count)
{
    unsigned n = 0;

    /* Pack four little-endian descriptor type bytes into independent lanes.
     * Only 0/1 (CPU descriptors) and 15 (hardware writeback) are supported.
     * Each add is below 256, so carries cannot cross byte lanes.
     */
    for (; count - n >= 4; n += 4) {
        const uint8_t *p = snapshot + n * 16 + 8;
        uint32_t types = (p[0] & 15) | ((p[16] & 15) << 8) |
                         ((p[32] & 15) << 16) | ((p[48] & 15) << 24);
        uint32_t not01 = ((types & UINT32_C(0x0e0e0e0e)) +
                          UINT32_C(0x7f7f7f7f)) & UINT32_C(0x80808080);
        uint32_t not15 = ((types ^ UINT32_C(0x0f0f0f0f)) +
                          UINT32_C(0x7f7f7f7f)) & UINT32_C(0x80808080);

        if (not01 & not15) {
            return false;
        }
    }
    for (; n < count; n++) {
        unsigned type = snapshot[n * 16 + 8] & 15;

        if (type != 0 && type != 1 && type != 15) {
            return false;
        }
    }
    return true;
}

/*
 * Data-vector interrupts and native data doorbells need only data queues.
 * Control-vector interrupts and unknown operations retain the full DMA path.
 * Capture every completion descriptor before publishing any payload, then
 * publish those immutable descriptors only after all selected payloads.
 */
int loom_iavf_sync_data_completion(VFIODevice *dev)
{
    static int scoped = -1;
    LoomIAVFQueues *s = dev->loom_iavf_queues;
    unsigned done[VF_QUEUE_COUNT] = { 0 }, devices = 0;
    VFIODevice *member;
    int ret;

    if (scoped < 0) {
        const char *mode = getenv("LOOM_IAVF_COMPLETION_SCOPED");

        scoped = !mode || strcmp(mode, "0");
    }
    if (!scoped || !dev->loom_iavf_irq_control || !dev->bcontainer ||
        !s || !s->valid_mask || !(s->enabled_rx | s->enabled_tx) ||
        s->pending || s->enable_pending || s->ambiguous ||
        !s->completion_payload) {
        return 0;
    }
    /* A container shared with another device needs its complete DMA domain. */
    QLIST_FOREACH(member, &dev->bcontainer->device_list, container_next) {
        devices++;
    }
    if (devices != 1) {
        return 0;
    }
    loom_iavf_reconcile_completion(dev);
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        LoomIAVFQueue *q = &s->queue[id];
        LoomIAVFCompletion *c = &s->completion[id];

        if (s->enabled_tx & (1U << id)) {
            if (!(s->valid_mask & (1U << id)) || !c->tx_snapshot ||
                !loom_vfio_dma_page_generation(dev->bcontainer, q->tx_iova) ||
                !loom_vfio_dma_range_covered(dev->bcontainer, q->tx_iova,
                                              q->tx_count * 16ULL)) {
                return 0;
            }
            ret = loom_vfio_dma_read_pool(dev->bcontainer, q->tx_iova,
                                          c->tx_snapshot, q->tx_count * 16ULL);
            if (ret) {
                return ret;
            }
            if (!tx_snapshot_types_valid(c->tx_snapshot, q->tx_count)) {
                return 0;
            }
        }
        if (!(s->enabled_rx & (1U << id))) {
            continue;
        }
        if (!(s->valid_mask & (1U << id)) || !c->seeded || !c->rx_slot ||
            !c->rx_snapshot || q->rx_buffer_size > 4096 ||
            !loom_vfio_dma_page_generation(dev->bcontainer, q->rx_iova) ||
            !loom_vfio_dma_range_covered(dev->bcontainer, q->rx_iova,
                                          q->rx_count * 32ULL)) {
            return 0;
        }
        for (unsigned n = 0; n < q->rx_count; n++) {
            unsigned slot = (c->rx_next + n) % q->rx_count;
            LoomIAVFRXSlot *posted = &c->rx_slot[slot];
            uint8_t status[8], *snapshot = c->rx_snapshot + slot * 32;
            uint64_t word;

            if (!posted->iova) {
                break;
            }
            if (!rx_slot_valid(dev, q, posted) ||
                payload_intersects_descriptors(s, posted->iova,
                                                 q->rx_buffer_size)) {
                c->seeded = false;
                return 0;
            }
            ret = loom_vfio_dma_read_pool(dev->bcontainer,
                                q->rx_iova + slot * 32ULL + 8, status, 8);
            if (ret) {
                return ret;
            }
            word = ldq_le_p(status);
            if (!(word & 1)) {
                break;
            }
            /*
             * read_pool's DMA read barrier orders the following descriptor
             * fields after DD, as in the native guest driver's dma_rmb().
             */
            ret = loom_vfio_dma_read_pool(dev->bcontainer,
                                q->rx_iova + slot * 32ULL, snapshot, 32);
            if (ret) {
                return ret;
            }
            if (ldq_le_p(snapshot + 8) != word ||
                ((word >> 38) & 0x3fff) > q->rx_buffer_size ||
                (word >> 52)) {
                c->seeded = false;
                return 0;
            }
            done[id]++;
        }
    }
    /* No guest writes until every descriptor, lifetime and range is checked. */
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        LoomIAVFQueue *q = &s->queue[id];
        LoomIAVFCompletion *c = &s->completion[id];

        for (unsigned n = 0; n < done[id]; n++) {
            unsigned slot = (c->rx_next + n) % q->rx_count;
            uint64_t address = c->rx_slot[slot].iova;

            ret = loom_vfio_dma_read_pool(dev->bcontainer, address,
                                  s->completion_payload, q->rx_buffer_size);
            if (!ret) {
                ret = loom_vfio_dma_publish_snapshot(dev->bcontainer, address,
                                  s->completion_payload, q->rx_buffer_size);
            }
            if (ret) {
                return ret;
            }
        }
    }
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        LoomIAVFQueue *q = &s->queue[id];
        LoomIAVFCompletion *c = &s->completion[id];

        if (s->enabled_tx & (1U << id)) {
            ret = loom_vfio_dma_publish_snapshot(dev->bcontainer, q->tx_iova,
                                          c->tx_snapshot, q->tx_count * 16ULL);
            if (ret) {
                return ret;
            }
        }
        for (unsigned n = 0; n < done[id]; n++) {
            unsigned slot = c->rx_next;

            uint64_t descriptor = q->rx_iova + slot * 32ULL;
            uint8_t *snapshot = c->rx_snapshot + slot * 32;

            /*
             * merge_buffer writes changed byte runs independently. Publish
             * metadata/length before the byte containing DD, so a concurrent
             * guest cannot see completion with an old length or RSS field.
             */
            ret = loom_vfio_dma_publish_snapshot(dev->bcontainer,
                                                  descriptor, snapshot, 8);
            if (!ret) {
                ret = loom_vfio_dma_publish_snapshot(dev->bcontainer,
                                         descriptor + 9, snapshot + 9, 23);
            }
            if (!ret) {
                smp_wmb();
                ret = loom_vfio_dma_publish_snapshot(dev->bcontainer,
                                         descriptor + 8, snapshot + 8, 1);
            }
            if (ret) {
                return ret;
            }
            memset(&c->rx_slot[slot], 0, sizeof(c->rx_slot[slot]));
            c->rx_next = (slot + 1) % q->rx_count;
        }
    }
    return 1;
}

/*
 * Physical AdminQ dependency preparation. These functions never write a BAR;
 * the enable/tail gate may proceed only after this callback returns zero.
 */
static int physical_stage(VFIODevice *dev, uint64_t addr, uint64_t size)
{
    int ret = loom_vfio_ram_materialize(dev->bcontainer, addr, size, true);

    return ret ? ret :
        loom_vfio_dma_sync_to_device_range(dev->bcontainer, addr, size);
}

/* Descriptor bytes have already been captured and validated under BQL. */
static int physical_stage_snapshot(VFIODevice *dev, uint64_t addr,
                                    const void *snapshot, uint64_t size)
{
    int ret = loom_vfio_ram_materialize(dev->bcontainer, addr, size, true);

    return ret ? ret :
        loom_vfio_dma_submit_snapshot(dev->bcontainer, addr, snapshot, size);
}

static bool physical_inline_op(unsigned op)
{
    switch (op) {
    case 1: case 2: case 3: case 7: case 9:
    case 10: case 11: case 12: case 13: case 14: case 15: case 18:
    case 23: case 24: case 25: case 26: case 27: case 28: case 29:
    case 44: case 51: case 52: case 53: case 54: case 55: case 56: case 57:
        return true;
    default:
        return false;
    }
}

static int physical_prepare_rx_queue(VFIODevice *dev, LoomIAVFQueues *s,
                                      const LoomIAVFQueue *q)
{
    g_autofree uint8_t *desc = NULL;
    int ret;

    /* Match the native non-header-split completion path's supported layout. */
    if (!q->rx_buffer_size || q->rx_buffer_size > 4096) {
        return -ENOTSUP;
    }
    desc = g_malloc(q->rx_count * 32ULL);
    /* Authorized CPU reads are bounded to 64 KiB each, including 4096 slots. */
    for (unsigned n = 0; n < q->rx_count; n += 2048) {
        unsigned count = MIN(q->rx_count - n, 2048);

        ret = loom_vfio_ram_read_cpu(dev->bcontainer, q->rx_iova + n * 32ULL,
                                     desc + n * 32, count * 32ULL);
        if (ret) {
            return ret;
        }
    }
    for (unsigned n = 0; n < q->rx_count; n++) {
        uint64_t address = ldq_le_p(desc + n * 32);

        /*
         * Native startup leaves an unposted zero slot (DESC_UNUSED is N-1).
         * Do not invent a DMA address for it. The BAR adapter must separately
         * validate every slot exposed by a data tail before forwarding it.
         */
        if (!address) {
            if (ldq_le_p(desc + n * 32 + 8) ||
                ldq_le_p(desc + n * 32 + 16) ||
                ldq_le_p(desc + n * 32 + 24)) {
                return -ENOTSUP;
            }
            continue;
        }
        if (ldq_le_p(desc + n * 32 + 8) ||
            address > UINT64_MAX - q->rx_buffer_size ||
            payload_intersects_descriptors(s, address, q->rx_buffer_size)) {
            return -ENOTSUP;
        }
    }
    for (unsigned n = 0; n < q->rx_count; n++) {
        uint64_t address = ldq_le_p(desc + n * 32);

        if (!address) {
            continue;
        }
        ret = physical_stage(dev, address, q->rx_buffer_size);
        if (ret) {
            return ret;
        }
    }
    /* All RX payloads precede the descriptors that expose them to hardware. */
    return physical_stage(dev, q->rx_iova, q->rx_count * 32ULL);
}

static bool physical_queue_layout(LoomIAVFQueues *s,
                                   const LoomIAVFQueue *candidate, uint16_t mask)
{
    for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
        const LoomIAVFQueue *q = &candidate[id];
        uint64_t address[2] = { q->tx_iova, q->rx_iova };
        uint64_t size[2] = { q->tx_count * 16ULL, q->rx_count * 32ULL };

        if (!(mask & (1U << id))) {
            continue;
        }
        for (unsigned r = 0; r < 2; r++) {
            if (range_intersects(address[r], size[r], s->asq_base,
                                  s->asq_count * AQ_DESC_SIZE) ||
                range_intersects(address[r], size[r], s->arq_base,
                                  s->arq_count * AQ_DESC_SIZE)) {
                return false;
            }
            for (unsigned other = id; other < VF_QUEUE_COUNT; other++) {
                const LoomIAVFQueue *o = &candidate[other];

                if (!(mask & (1U << other))) {
                    continue;
                }
                if ((other != id || r != 0) &&
                    range_intersects(address[r], size[r], o->tx_iova,
                                      o->tx_count * 16ULL)) {
                    return false;
                }
                if ((other != id || r != 1) &&
                    range_intersects(address[r], size[r], o->rx_iova,
                                      o->rx_count * 32ULL)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static int physical_prepare_request(VFIODevice *dev, LoomIAVFQueues *s,
                                     const uint8_t *desc)
{
    unsigned opcode = lduw_le_p(desc + 2), op = ldl_le_p(desc + 8);
    unsigned flags = lduw_le_p(desc), len = lduw_le_p(desc + 4);
    uint64_t address = (uint64_t)ldl_le_p(desc + 24) << 32 |
                       ldl_le_p(desc + 28);
    uint8_t msg[72 + VF_QUEUE_COUNT * 64];
    int ret;

    if ((flags & 7) || (len && !(flags & 0x1000)) ||
        ((flags & 0x1000) && (!len || !address)) ||
        (len && address > UINT64_MAX - len)) {
        return -EINVAL;
    }
    if (opcode != 0x0801 && opcode != 1 && opcode != 2 && opcode != 3) {
        return -ENOTSUP;
    }
    if (opcode == 0x0801 &&
        op != 6 && op != 8 && !physical_inline_op(op)) {
        return -ENOTSUP;
    }
    if (len && payload_intersects_descriptors(s, address, len)) {
        return -ENOTSUP;
    }
    if (opcode == 0x0801 && (op == 6 || op == 8)) {
        if (!(flags & 0x1000) || !(flags & 0x400) ||
            len > sizeof(msg)) {
            return -EINVAL;
        }
        ret = loom_vfio_ram_read_cpu(dev->bcontainer, address, msg, len);
        if (ret) {
            return ret;
        }
        if (op == 6) {
            LoomIAVFQueue candidate[VF_QUEUE_COUNT] = { 0 };
            uint16_t mask, vsi;

            if (s->pending || s->enable_pending || s->ambiguous ||
                s->enabled_rx || s->enabled_tx ||
                !queue_config_parse(candidate, &mask, &vsi, msg, len) ||
                !physical_queue_layout(s, candidate, mask)) {
                return -ENOTSUP;
            }
            for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
                const LoomIAVFQueue *q = &candidate[id];

                if (!(mask & (1U << id))) {
                    continue;
                }
                ret = physical_stage(dev, q->tx_iova, q->tx_count * 16ULL);
                if (!ret) {
                    ret = physical_stage(dev, q->rx_iova, q->rx_count * 32ULL);
                }
                if (ret) {
                    return ret;
                }
            }
        } else {
            uint32_t rx, tx;

            if (len != 12 || lduw_le_p(msg) != s->vsi ||
                lduw_le_p(msg + 2) || !s->valid_mask || s->pending ||
                s->enable_pending || s->ambiguous) {
                return -ENOTSUP;
            }
            rx = ldl_le_p(msg + 4);
            tx = ldl_le_p(msg + 8);
            if (!(rx | tx) || ((rx | tx) & ~s->valid_mask)) {
                return -EINVAL;
            }
            if ((rx & s->enabled_rx) || (tx & s->enabled_tx)) {
                return -ENOTSUP;
            }
            for (unsigned id = 0; id < VF_QUEUE_COUNT; id++) {
                if (rx & (1U << id)) {
                    ret = physical_prepare_rx_queue(dev, s, &s->queue[id]);
                    if (ret) {
                        return ret;
                    }
                }
                if (tx & (1U << id)) {
                    ret = physical_stage(dev, s->queue[id].tx_iova,
                                           s->queue[id].tx_count * 16ULL);
                    if (ret) {
                        return ret;
                    }
                }
            }
        }
    }
    /* RAM authority is bidirectional even when this command uses AQ_FLAG_RD. */
    return len ? physical_stage(dev, address, len) : 0;
}

int loom_iavf_prepare_physical_admin(VFIODevice *dev, bool arq, bool enabling,
                                    uint64_t base, unsigned count,
                                    unsigned next, unsigned tail)
{
    LoomIAVFQueues *s;
    unsigned span, first;
    uint8_t *desc;
    int ret;

    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram ||
        !dev->loom_iavf_irq_control) {
        return -ENOTSUP;
    }
    if (!count || count >= AQ_MAX_COUNT || !base || (base & 63) ||
        base > UINT64_MAX - count * AQ_DESC_SIZE ||
        next >= count || tail >= count) {
        return -EINVAL;
    }
    s = queues_get(dev);
    if (!s) {
        return -ENOTSUP;
    }
    if (base != (arq ? s->arq_base : s->asq_base) ||
        count != (arq ? s->arq_count : s->asq_count)) {
        return -ESTALE;
    }
    if (enabling && !arq) {
        /* Unsubmitted ASQ entries have no external payload dependency. */
        return physical_stage(dev, base, count * AQ_DESC_SIZE);
    }
    span = enabling ? count :
           (tail + count - next) % count + (arq ? 1 : 0);
    if (!span) {
        return 0;
    }
    if (enabling) {
        next = 0;
    }
    first = MIN(span, count - next);
    desc = s->submit_desc;
    ret = loom_vfio_ram_read_cpu(dev->bcontainer, base + next * AQ_DESC_SIZE,
                                 desc, first * AQ_DESC_SIZE);
    if (!ret && first < span) {
        ret = loom_vfio_ram_read_cpu(dev->bcontainer, base,
                                     desc + first * AQ_DESC_SIZE,
                                     (span - first) * AQ_DESC_SIZE);
    }
    if (ret) {
        return ret;
    }
    if (!arq && span > 1) {
        /* The observer correlates one outstanding queue transition at a time. */
        for (unsigned n = 0; n < span; n++) {
            const uint8_t *d = desc + n * AQ_DESC_SIZE;
            unsigned op = ldl_le_p(d + 8);

            if (lduw_le_p(d + 2) == 0x0801 &&
                (op == 2 || op == 6 || op == 8 || op == 9 || op == 29)) {
                return -ENOTSUP;
            }
        }
    }
    for (unsigned n = 0; n < span; n++) {
        const uint8_t *d = desc + n * AQ_DESC_SIZE;

        if (arq) {
            unsigned len = lduw_le_p(d + 4);
            uint64_t address = (uint64_t)ldl_le_p(d + 24) << 32 |
                               ldl_le_p(d + 28);

            if (!(lduw_le_p(d) & 0x1000) || !len || !address ||
                address > UINT64_MAX - len ||
                range_intersects(address, len, base, count * AQ_DESC_SIZE) ||
                payload_intersects_descriptors(s, address, len)) {
                return -ENOTSUP;
            }
            ret = physical_stage(dev, address, len);
        } else {
            ret = physical_prepare_request(dev, s, d);
        }
        if (ret) {
            return ret;
        }
    }
    ret = physical_stage(dev, base + next * AQ_DESC_SIZE,
                           first * AQ_DESC_SIZE);
    if (!ret && first < span) {
        ret = physical_stage(dev, base, (span - first) * AQ_DESC_SIZE);
    }
    return ret;
}

/* Sparse data queues: never forward a tail whose new DMA addresses are absent. */
static int physical_read_ring(VFIODevice *dev, uint64_t base, unsigned entries,
                               unsigned stride, unsigned start, unsigned count,
                               uint8_t *desc)
{
    unsigned copied = 0;

    while (copied < count) {
        unsigned pos = (start + copied) % entries;
        unsigned n = MIN(count - copied, MIN(entries - pos, 65536 / stride));
        int ret = loom_vfio_ram_read_cpu(dev->bcontainer, base + pos * stride,
                                         desc + copied * stride, n * stride);

        if (ret) {
            return ret;
        }
        copied += n;
    }
    return 0;
}

int loom_iavf_prepare_physical_data(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    LoomIAVFQueues *s;
    LoomIAVFQueue *q;
    bool rx;
    unsigned id, entries, stride, old, span, first;
    uint64_t base;
    uint8_t *desc, sentinel[8];
    uint64_t *payload;
    uint32_t *length;
    int ret;

    if (addr <= 0x3c && !(addr & 3)) {
        rx = false;
        id = addr / 4;
    } else if (addr >= 0x2000 && addr <= 0x203c && !(addr & 3)) {
        rx = true;
        id = (addr - 0x2000) / 4;
    } else {
        return 0;
    }
    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram ||
        !dev->loom_iavf_irq_control) {
        return 0;
    }
    s = queues_get(dev);
    if (!s || s->ambiguous || value >= 4096) {
        return -EINVAL;
    }
    if (!(s->valid_mask & (1U << id))) {
        if (s->enabled_rx || s->enabled_tx || s->enable_pending) {
            return -ENOTSUP;
        }
        if (rx) {
            /* Native RX initialization precedes PF CONFIG_VSI_QUEUES. */
            s->physical.deferred_rx[id] = value;
            s->physical.deferred_mask |= 1U << id;
            return 2;
        }
        /* An initial TX clear exposes no descriptors; no later flush needed. */
        return value ? -ENOTSUP : 2;
    }
    if (s->pending ||
        ((rx ? s->physical.rx_prepared_mask : s->physical.tx_prepared_mask) &
         (1U << id))) {
        return -EBUSY;
    }
    q = &s->queue[id];
    entries = rx ? q->rx_count : q->tx_count;
    stride = rx ? 32 : 16;
    base = rx ? q->rx_iova : q->tx_iova;
    if (!entries || entries > 4096 || (entries % (rx ? 32 : 8)) ||
        !base || (base & 127) || base > UINT64_MAX - entries * stride ||
        value >= entries) {
        return -ESTALE;
    }
    if (!rx && value &&
        !((s->enabled_tx | s->pending_tx) & (1U << id))) {
        return -ENOTSUP;
    }
    old = ((rx ? s->rx_tail_known : s->tx_tail_known) & (1U << id)) ?
          (rx ? s->rx_tail[id] : s->tx_tail[id]) : 0;
    if (old >= entries || (rx && (!q->rx_buffer_size ||
                                 q->rx_buffer_size > 4096))) {
        return -ESTALE;
    }
    span = (value + entries - old) % entries;
    first = MIN(span, entries - old);
    desc = s->submit_desc;
    payload = s->submit_payload;
    length = s->tx_length;
    ret = physical_read_ring(dev, base, entries, stride, old, span, desc);
    if (ret) {
        return ret;
    }
    for (unsigned n = 0; n < span; n++) {
        const uint8_t *d = desc + n * stride;

        if (rx) {
            if (ldq_le_p(d + 8)) {
                return -ENOTSUP;
            }
            payload[n] = ldq_le_p(d);
            length[n] = q->rx_buffer_size;
        } else {
            uint64_t command = ldq_le_p(d + 8);
            unsigned type = command & 15;

            length[n] = 0;
            if (type == 1) {
                /* Native TSO/VLAN context contains no external DMA address. */
                if (lduw_le_p(d + 6) ||
                    (((command >> 4) & 0xffff) & ~0x7fU) ||
                    (command & ((UINT64_C(0x3ff) << 20) |
                                (UINT64_C(3) << 48)))) {
                    return -ENOTSUP;
                }
                continue;
            }
            if (type != 0) {
                return -ENOTSUP;
            }
            payload[n] = ldq_le_p(d);
            length[n] = (command >> 34) & 0x3fff;
        }
        if (!payload[n] || !length[n] ||
            payload[n] > UINT64_MAX - length[n] ||
            payload_intersects_descriptors(s, payload[n], length[n])) {
            return -ENOTSUP;
        }
    }
    if (rx) {
        /* NTU is exclusive: its cleared status is not a payload address. */
        ret = loom_vfio_ram_read_cpu(dev->bcontainer,
                                     base + value * 32ULL + 8,
                                     sentinel, sizeof(sentinel));
        if (ret) {
            return ret;
        }
        if (ldq_le_p(sentinel)) {
            return -ENOTSUP;
        }
    }
    for (unsigned n = 0; n < span; n++) {
        if (!length[n]) {
            continue;
        }
        ret = physical_stage(dev, payload[n], length[n]);
        if (ret) {
            return ret;
        }
    }
    if (span) {
        ret = physical_stage_snapshot(dev, base + old * stride, desc,
                                      first * stride);
        if (!ret && first < span) {
            ret = physical_stage_snapshot(dev, base, desc + first * stride,
                                           (span - first) * stride);
        }
        if (ret) {
            return ret;
        }
    }
    if (rx) {
        ret = physical_stage_snapshot(dev, base + value * 32ULL + 8,
                                      sentinel, sizeof(sentinel));
        if (ret) {
            return ret;
        }
        s->physical.prepared_rx[id] = value;
        s->physical.rx_prepared_mask |= 1U << id;
    } else {
        s->physical.prepared_tx[id] = value;
        s->physical.tx_prepared_mask |= 1U << id;
    }
    return 1;
}

/*
 * Seed completion tracking before DMA can begin, without committing the tail.
 * The ordinary observer advances next; retain it for the legacy path only.
 */
int loom_iavf_record_physical_data(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    LoomIAVFQueues *s;
    bool rx;
    unsigned id;
    uint16_t old, known;

    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram ||
        !(s = dev->loom_iavf_queues)) {
        return -ENOTSUP;
    }
    if (addr >= 0x2000 && addr <= 0x203c && !(addr & 3)) {
        rx = true;
        id = (addr - 0x2000) / 4;
    } else if (addr <= 0x3c && !(addr & 3)) {
        rx = false;
        id = addr / 4;
    } else {
        return -EINVAL;
    }
    if (!((rx ? s->physical.rx_prepared_mask :
                s->physical.tx_prepared_mask) & (1U << id)) ||
        (rx ? s->physical.prepared_rx[id] :
              s->physical.prepared_tx[id]) != value) {
        return -ESTALE;
    }
    old = rx ? s->rx_tail[id] : s->tx_tail[id];
    known = rx ? s->rx_tail_known : s->tx_tail_known;
    loom_iavf_observe_write(dev, addr, value);
    if (rx) {
        s->rx_tail[id] = old;
        s->rx_tail_known = known;
    } else {
        s->tx_tail[id] = old;
        s->tx_tail_known = known;
    }
    return 0;
}

int loom_iavf_commit_physical_data(VFIODevice *dev, hwaddr addr, uint32_t value)
{
    LoomIAVFQueues *s;
    unsigned id;

    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram ||
        !(s = dev->loom_iavf_queues)) {
        return -ENOTSUP;
    }
    if (addr >= 0x2000 && addr <= 0x203c && !(addr & 3)) {
        id = (addr - 0x2000) / 4;
        if (!(s->physical.rx_prepared_mask & (1U << id)) ||
            s->physical.prepared_rx[id] != value) {
            return -ESTALE;
        }
        s->physical.rx_prepared_mask &= ~(1U << id);
        s->rx_tail[id] = value;
        s->rx_tail_known |= 1U << id;
        if (s->physical.deferred_rx[id] == value) {
            s->physical.deferred_mask &= ~(1U << id);
        }
    } else if (addr <= 0x3c && !(addr & 3)) {
        id = addr / 4;
        if (!(s->physical.tx_prepared_mask & (1U << id)) ||
            s->physical.prepared_tx[id] != value) {
            return -ESTALE;
        }
        s->physical.tx_prepared_mask &= ~(1U << id);
        s->tx_tail[id] = value;
        s->tx_tail_known |= 1U << id;
    } else {
        return -EINVAL;
    }
    return 0;
}

bool loom_iavf_deferred_physical_rx(VFIODevice *dev, unsigned id, uint32_t *value)
{
    LoomIAVFQueues *s;

    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram || !value ||
        id >= VF_QUEUE_COUNT || !(s = dev->loom_iavf_queues) ||
        !(s->physical.deferred_mask & (1U << id))) {
        return false;
    }
    *value = s->physical.deferred_rx[id];
    return true;
}


int loom_iavf_physical_rx_configuration(VFIODevice *dev)
{
    LoomIAVFQueues *s;

    if (!dev || !dev->bcontainer || !dev->bcontainer->loom_ram ||
        !(s = dev->loom_iavf_queues)) {
        return -ENOTSUP;
    }
    if (s->ambiguous) {
        return -ESTALE;
    }
    if (s->pending || !s->valid_mask) {
        return 0;
    }
    if (s->physical.deferred_mask & ~s->valid_mask) {
        return -ESTALE;
    }
    return 1;
}
