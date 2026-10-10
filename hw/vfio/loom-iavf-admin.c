/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gate physical X710 VF AdminQ enables until descriptor dependencies exist.
 * LEN can precede BAL/BAH in the unmodified iavf driver. Partial configuration
 * must therefore never expose an enabled queue to hardware.
 */
#include "qemu/osdep.h"
#include "hw/vfio/loom-iavf-admin.h"

#define AQ_ENABLE (1U << 31)
#define AQ_LENGTH_MASK (AQ_ENABLE | 0x3ffU)
#define AQ_CONFIG_ALL 7

enum {
    AQ_LENGTH, AQ_LOW, AQ_HIGH, AQ_HEAD, AQ_TAIL,
};

static const hwaddr aq_register[2][5] = {
    { 0x6800, 0x7c00, 0x7800, 0x6400, 0x8400 },
    { 0x8000, 0x6c00, 0x6000, 0x7400, 0x7000 },
};

static bool aq_lookup(hwaddr addr, unsigned *id, unsigned *reg)
{
    for (unsigned q = 0; q < 2; q++) {
        for (unsigned r = 0; r < 5; r++) {
            if (addr == aq_register[q][r]) {
                *id = q;
                *reg = r;
                return true;
            }
        }
    }
    return false;
}

/* A successful disable includes a posted-write flush and enable readback. */
static int aq_stop(LoomIAVFAdminState *s, unsigned id,
                    const LoomIAVFAdminOps *ops, void *opaque, bool force)
{
    LoomIAVFAdminQueue *q = &s->queue[id];
    uint32_t readback;
    int ret;

    if (!force && !q->hardware_enabled) {
        return 0;
    }
    ret = ops->write(opaque, aq_register[id][AQ_LENGTH],
                     q->length & ~AQ_ENABLE);
    if (!ret) {
        ret = ops->read(opaque, aq_register[id][AQ_LENGTH], &readback);
    }
    if (!ret && (readback & AQ_ENABLE)) {
        ret = -EIO;
    }
    if (!ret) {
        q->hardware_enabled = false;
    }
    return ret;
}

static int aq_fail(LoomIAVFAdminState *s, int ret,
                    const LoomIAVFAdminOps *ops, void *opaque)
{
    s->failed = true;
    /* Also disable a queue whose failed enable write may have reached hardware. */
    aq_stop(s, 0, ops, opaque, true);
    aq_stop(s, 1, ops, opaque, true);
    return ret < 0 ? ret : -EIO;
}

static int aq_enable(LoomIAVFAdminState *s, unsigned id,
                      const LoomIAVFAdminOps *ops, void *opaque)
{
    LoomIAVFAdminQueue *q = &s->queue[id];
    unsigned count = q->length & 0x3ff;
    uint32_t readback;
    int ret;

    if (!(q->length & AQ_ENABLE) || q->configured != AQ_CONFIG_ALL) {
        return 0;
    }
    if (!count || !q->base || (q->base & 63) ||
        q->base > UINT64_MAX - count * 32ULL) {
        return -EINVAL;
    }
    ret = ops->prepare(opaque, id == 1, true, q->base, count, 0, 0);
    if (ret) {
        return ret;
    }
    ret = ops->write(opaque, aq_register[id][AQ_LENGTH], q->length);
    if (!ret) {
        ret = ops->read(opaque, aq_register[id][AQ_LENGTH], &readback);
    }
    if (!ret && (readback & AQ_LENGTH_MASK) != q->length) {
        ret = -EIO;
    }
    if (!ret) {
        q->hardware_enabled = true;
        q->next = 0;
    }
    return ret;
}

int loom_iavf_admin_write(LoomIAVFAdminState *s, hwaddr addr, uint32_t value,
                          const LoomIAVFAdminOps *ops, void *opaque)
{
    LoomIAVFAdminQueue *q;
    unsigned id, reg;
    uint32_t physical = value;
    int ret;

    if (!aq_lookup(addr, &id, &reg)) {
        return 0;
    }
    if (!s || !ops || !ops->write || !ops->read || !ops->prepare) {
        return -EINVAL;
    }
    if (s->failed) {
        return -EIO;
    }
    q = &s->queue[id];

    if (reg == AQ_HEAD) {
        /* This prototype accepts the native driver's clear-head sequence. */
        if (value) {
            return aq_fail(s, -ENOTSUP, ops, opaque);
        }
        ret = aq_stop(s, id, ops, opaque, false);
        if (!ret) {
            q->configured = 0;
            q->next = 0;
            ret = ops->write(opaque, addr, value);
        }
        return ret ? aq_fail(s, ret, ops, opaque) : 1;
    }
    if (reg == AQ_TAIL) {
        unsigned count = q->length & 0x3ff;

        if (!q->hardware_enabled) {
            /* Initial/shutdown tail clear cannot start an unprepared queue. */
            ret = value ? -EINVAL : ops->write(opaque, addr, 0);
        } else if (value >= count || q->next >= count) {
            ret = -EINVAL;
        } else {
            ret = ops->prepare(opaque, id == 1, false, q->base, count,
                               q->next, value);
            if (!ret) {
                ret = ops->write(opaque, addr, value);
            }
            if (!ret) {
                q->next = id == 1 ? (value + 1) % count : value;
            }
        }
        return ret ? aq_fail(s, ret, ops, opaque) : 1;
    }

    if (reg == AQ_LENGTH && (value & ~AQ_LENGTH_MASK)) {
        return aq_fail(s, -ENOTSUP, ops, opaque);
    }
    if (q->hardware_enabled) {
        /* Every config cycle needs fresh LEN/BAL/BAH, even unchanged halves. */
        ret = aq_stop(s, id, ops, opaque, false);
        if (ret) {
            return aq_fail(s, ret, ops, opaque);
        }
        q->configured = 0;
        q->next = 0;
    }
    if (reg == AQ_LENGTH) {
        q->length = value;
        q->configured |= 1;
        physical &= ~AQ_ENABLE;
    } else if (reg == AQ_LOW) {
        q->base = (q->base & ~UINT64_C(0xffffffff)) | value;
        q->configured |= 2;
    } else {
        q->base = (q->base & UINT32_MAX) | (uint64_t)value << 32;
        q->configured |= 4;
    }
    ret = ops->write(opaque, addr, physical);
    if (!ret) {
        ret = aq_enable(s, id, ops, opaque);
    }
    return ret ? aq_fail(s, ret, ops, opaque) : 1;
}

bool loom_iavf_admin_read(const LoomIAVFAdminState *s, hwaddr addr,
                          uint32_t *value)
{
    unsigned id, reg;

    if (!s || !value || !aq_lookup(addr, &id, &reg) || reg != AQ_LENGTH) {
        return false;
    }
    /* Once running, expose a hardware reset/error rather than inventing alive. */
    if (s->failed || s->queue[id].hardware_enabled) {
        return true;
    }
    *value = (*value & ~AQ_LENGTH_MASK) | s->queue[id].length;
    return true;
}
