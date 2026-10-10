/*
 * Loom owner-based PCI aggregation over the GVM RDMA router.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "exec/address-spaces.h"
#include "hw/boards.h"
#include "hw/pci/loom.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_host.h"
#include "pci-internal.h"
#include "hw/qdev-properties.h"
#include "interrupt-router.h"
#include "migration/rdma-gvm.h"
#include <linux/kvm.h>

#define TYPE_LOOM_REMOTE_PCI "loom-remote-pci"
#define LOOM_MAX_DEVICES 64
#define LOOM_MAX_NODES 4
#define LOOM_DIRECTORY_MAGIC 0x4c4f4f4d50434931ULL
#define LOOM_DIRECTORY_VERSION 3

typedef struct LoomBAR {
    uint64_t address;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
} LoomBAR;

typedef struct LoomPCIDescriptor {
    uint64_t sequence;
    uint32_t generation;
    uint16_t bdf;
    uint16_t config_size;
    int32_t numa_node;
    uint32_t cap_present;
    LoomBAR bars[PCI_NUM_REGIONS];
    uint8_t config[PCIE_CONFIG_SPACE_SIZE];
} LoomPCIDescriptor;

typedef struct LoomPCINode {
    uint64_t magic;
    uint32_t version;
    uint32_t ready;
    uint32_t count;
    uint32_t reserved;
    uint64_t heartbeat;
    LoomPCIDescriptor devices[LOOM_MAX_DEVICES];
} LoomPCINode;

typedef struct LoomRemotePCI {
    PCIDevice parent_obj;
    uint32_t device_id;
    uint32_t generation;
    MemoryRegion bars[PCI_NUM_REGIONS];
    struct {
        struct LoomRemotePCI *device;
        unsigned bar;
    } bar_context[PCI_NUM_REGIONS];
} LoomRemotePCI;

static LoomPCINode *directory;
/* Incoming publication has its own lock: the CQ receiver never needs BQL. */
static LoomPCINode *incoming_directory;
static bool incoming_valid[LOOM_MAX_NODES][LOOM_MAX_DEVICES];
static QemuMutex directory_lock;
static QemuCond directory_changed;
static uint64_t publication_serial;
static uint64_t expected_ack[LOOM_MAX_NODES], received_ack[LOOM_MAX_NODES];
static int ack_status[LOOM_MAX_NODES];
static bool directory_control_active;
static uint64_t directory_epoch, applied_directory_epoch;
static void publish_directory_update(unsigned index);
static void snapshot(LoomPCIDescriptor *src, LoomPCIDescriptor *dst);

static PCIDevice *owned[LOOM_MAX_DEVICES];
static uint32_t owned_generation[LOOM_MAX_DEVICES];
static unsigned owned_reset_depth[LOOM_MAX_DEVICES];
static PCIBus *root_bus;
static unsigned local_node, node_count, owned_count;
static uint32_t config_address;

G_STATIC_ASSERT(sizeof(LoomPCIDescriptor) == 4288);
/* Only this prefix changes BAR mappings. Extended config reads use owner RPC. */
#define LOOM_DESCRIPTOR_PREFIX \
    (offsetof(LoomPCIDescriptor, config) + PCI_CONFIG_SPACE_SIZE)

G_STATIC_ASSERT(LOOM_DESCRIPTOR_PREFIX == LOOM_DIRECTORY_UPDATE_SIZE);

bool loom_pci_enabled(void)
{
    const char *value = getenv("LOOM_DEVICE_AGGREGATION");
    return value && !strcmp(value, "1");
}

bool loom_pci_intx_requires_proxy(PCIDevice *dev)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    return loom_pci_enabled() && dev->loom_aggregate &&
           ms->local_cpu_start_index != 0;
}

bool loom_pci_route_intx(PCIDevice *dev, unsigned pin, int change)
{
    unsigned index;

    if (!directory || !loom_pci_intx_requires_proxy(dev)) {
        return false;
    }
    if (!change) {
        return true;
    }
    for (index = 0; index < owned_count && owned[index] != dev; index++) {
    }
    if (index < owned_count) {
        LoomPCIRequest req = {
            .operation = LOOM_PCI_INTX,
            .device_id = local_node * LOOM_MAX_DEVICES + index,
            .generation = directory[local_node].devices[index].generation,
            .bar = pin,
            .value = change > 0,
        };

        loom_pci_intx_notify(&req);
        return true;
    }
    return false;
}

static void fatal_error(const char *message)
{
    error_report("Loom PCI: %s", message);
    exit(EXIT_FAILURE);
}

static int owned_index(PCIDevice *dev)
{
    for (unsigned i = 0; directory && i < owned_count; i++) {
        if (owned[i] == dev) {
            return i;
        }
    }
    return -1;
}

void loom_pci_reset_begin(PCIDevice *dev)
{
    int i = owned_index(dev);

    if (i < 0 || owned_reset_depth[i]++) {
        return;
    }
    if (owned_generation[i] == UINT32_MAX) {
        fatal_error("device generation exhausted; restart the aggregate");
    }
    owned_generation[i]++;
    /* An odd snapshot blocks new access while the owner quiesces/resets
     * the model. Reset callbacks run under its BQL, after earlier RPCs.
     */
    qatomic_store_release(&directory[local_node].devices[i].sequence,
                          qatomic_read(&directory[local_node].devices[i].sequence) | 1);
    publish_directory_update(i);
}

void loom_pci_reset_end(PCIDevice *dev)
{
    int i = owned_index(dev);

    if (i < 0 || !owned_reset_depth[i] || --owned_reset_depth[i]) {
        return;
    }
    loom_pci_publish(dev);
    if (local_node) {
        for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
            LoomPCIRequest req = {
                .operation = LOOM_PCI_INTX,
                .device_id = local_node * LOOM_MAX_DEVICES + i,
                .generation = owned_generation[i],
                .bar = pin, .value = 0,
            };

            loom_pci_intx_notify(&req);
        }
    }
}

bool loom_pci_config_will_reset(PCIDevice *dev, uint32_t addr,
                                uint32_t value, unsigned len)
{
    unsigned byte;
    uint8_t af;

    if (owned_index(dev) < 0) {
        return false;
    }
    if (pci_is_express(dev) &&
        (pci_get_long(dev->config + dev->exp.exp_cap + PCI_EXP_DEVCAP) & PCI_EXP_DEVCAP_FLR)) {
        byte = dev->exp.exp_cap + PCI_EXP_DEVCTL + 1;
        if (addr <= byte && byte - addr < len &&
            ((value >> (8 * (byte - addr))) & 0x80)) {
            return true;
        }
    }
    af = pci_find_capability(dev, PCI_CAP_ID_AF);
    byte = af + PCI_AF_CTRL;
    return af && (dev->config[af + PCI_AF_CAP] & PCI_AF_CAP_FLR) &&
           addr <= byte && byte - addr < len &&
           ((value >> (8 * (byte - addr))) & PCI_AF_CTRL_FLR);
}

static LoomPCIDescriptor *descriptor(unsigned id)
{
    unsigned owner = id / LOOM_MAX_DEVICES;
    unsigned index = id % LOOM_MAX_DEVICES;

    if (!directory || owner >= node_count ||
        !qatomic_load_acquire(&directory[owner].ready) ||
        index >= directory[owner].count) {
        return NULL;
    }
    return &directory[owner].devices[index];
}

void loom_pci_intx_dispatch(const LoomPCIRequest *req)
{
    bool locked = bql_locked();
    LoomPCIDescriptor *d = descriptor(req->device_id);
    LoomPCIDescriptor current;
    PCIDevice *dev;

    if (d && req->device_id / LOOM_MAX_DEVICES != local_node) {
        snapshot(d, &current);
        d = &current;
    }
    if (!d || local_node != 0 || req->operation != LOOM_PCI_INTX ||
        req->device_id / LOOM_MAX_DEVICES == local_node ||
        req->generation != d->generation || req->bar >= PCI_NUM_PINS ||
        req->value > 1) {
        error_report("Loom PCI: invalid or stale INTx notification");
        return;
    }
    if (!locked) {
        bql_lock();
    }
    dev = root_bus->devices[d->bdf];
    if (dev && object_dynamic_cast(OBJECT(dev), TYPE_LOOM_REMOTE_PCI)) {
        loom_pci_set_effective_irq(dev, req->bar, req->value);
    }
    if (!locked) {
        bql_unlock();
    }
}

/*
 * Startup pulls full descriptors. Runtime prefixes are published by the
 * owner and acknowledged before its control operation completes. Neither
 * shared host pointers nor cross-host atomic memory accesses are assumed.
 */
static void directory_read(unsigned owner, unsigned offset,
                           void *buffer, unsigned length)
{
    if (owner == local_node) {
        memcpy(buffer, (uint8_t *)&directory[owner] + offset, length);
    } else if (loom_pci_directory_rpc(owner, offset, buffer, length) < 0) {
        fatal_error("RDMA directory unavailable; aggregate must stop");
    }
}

static void snapshot(LoomPCIDescriptor *src, LoomPCIDescriptor *dst)
{
    unsigned owner, offset;
    bool locked = bql_locked();

    if (!src) {
        fatal_error("invalid device descriptor");
    }
    owner = ((uintptr_t)src - (uintptr_t)directory) / sizeof(LoomPCINode);
    offset = (uintptr_t)src - (uintptr_t)&directory[owner];
    if (!locked) {
        bql_lock();
    }
    *dst = *src;
    if (owner != local_node) {
        if (qatomic_load_acquire(&directory_control_active)) {
            unsigned index = (offset - offsetof(LoomPCINode, devices)) /
                             sizeof(LoomPCIDescriptor);
            int64_t deadline = g_get_monotonic_time() + 5000000;
            qemu_mutex_lock(&directory_lock);
            while (incoming_valid[owner][index] &&
                   (incoming_directory[owner].devices[index].sequence & 1)) {
                if (g_get_monotonic_time() >= deadline) {
                    qemu_mutex_unlock(&directory_lock);
                    fatal_error("owner reset publication timed out");
                }
                qemu_cond_timedwait(&directory_changed, &directory_lock, 100);
            }
            if (!incoming_valid[owner][index]) {
                qemu_mutex_unlock(&directory_lock);
                fatal_error("missing owner control snapshot");
            }
            *dst = incoming_directory[owner].devices[index];
            qemu_mutex_unlock(&directory_lock);
        } else {
            directory_read(owner, offset, dst, LOOM_DESCRIPTOR_PREFIX);
        }
        *src = *dst;
    }
    if (!locked) {
        bql_unlock();
    }
    if (dst->sequence & 1) {
        fatal_error("device reset snapshot unavailable; aggregate must stop");
    }
}

/* These functions execute on the independent receive/control path. */
int loom_pci_directory_update_dispatch(unsigned peer, unsigned device_id,
                                       const void *data, unsigned size)
{
    unsigned index = device_id % LOOM_MAX_DEVICES;
    LoomPCIDescriptor update = { 0 };
    LoomPCIDescriptor *cached;

    if (!directory || peer >= node_count || peer == local_node ||
        device_id / LOOM_MAX_DEVICES != peer ||
        size != LOOM_DESCRIPTOR_PREFIX) {
        return -EINVAL;
    }
    memcpy(&update, data, size);
    if ((update.config_size != PCI_CONFIG_SPACE_SIZE &&
         update.config_size != PCIE_CONFIG_SPACE_SIZE) ||
        update.bdf >= ARRAY_SIZE(root_bus->devices)) {
        return -EINVAL;
    }
    qemu_mutex_lock(&directory_lock);
    cached = &incoming_directory[peer].devices[index];
    if (!incoming_valid[peer][index] || update.sequence >= cached->sequence) {
        memcpy(cached, &update, size);
        incoming_valid[peer][index] = true;
        qatomic_inc(&directory_epoch);
        qemu_cond_broadcast(&directory_changed);
    }
    qemu_mutex_unlock(&directory_lock);
    return 0;
}

void loom_pci_directory_ack_dispatch(unsigned peer, uint64_t serial, int status)
{
    if (!directory || peer >= node_count || peer == local_node) {
        return;
    }
    qemu_mutex_lock(&directory_lock);
    if (serial == expected_ack[peer]) {
        received_ack[peer] = serial;
        ack_status[peer] = status;
        qemu_cond_broadcast(&directory_changed);
    }
    qemu_mutex_unlock(&directory_lock);
}

static void publish_directory_update(unsigned index)
{
    LoomPCIDescriptor update;
    uint64_t serial;

    if (!qatomic_load_acquire(&directory_control_active)) {
        return;
    }
    assert(bql_locked());
    if (publication_serial == UINT64_MAX) {
        fatal_error("directory publication serial exhausted");
    }
    serial = ++publication_serial;
    memcpy(&update, &directory[local_node].devices[index],
           LOOM_DESCRIPTOR_PREFIX);
    for (unsigned peer = 0; peer < node_count; peer++) {
        int64_t deadline;
        int status;
        if (peer == local_node) {
            continue;
        }
        qemu_mutex_lock(&directory_lock);
        expected_ack[peer] = serial;
        received_ack[peer] = 0;
        qemu_mutex_unlock(&directory_lock);
        if (loom_pci_directory_push(peer, serial,
                                    local_node * LOOM_MAX_DEVICES + index,
                                    &update, LOOM_DESCRIPTOR_PREFIX) < 0) {
            fatal_error("directory publication failed; aggregate must stop");
        }
        deadline = g_get_monotonic_time() + 5000000;
        qemu_mutex_lock(&directory_lock);
        while (received_ack[peer] != serial) {
            if (g_get_monotonic_time() >= deadline) {
                qemu_mutex_unlock(&directory_lock);
                fatal_error("directory acknowledgement timed out");
            }
            qemu_cond_timedwait(&directory_changed, &directory_lock, 100);
        }
        status = ack_status[peer];
        qemu_mutex_unlock(&directory_lock);
        if (status) {
            fatal_error("peer rejected directory publication");
        }
    }
}

void loom_pci_directory_dispatch(uint32_t offset, void *buffer, uint32_t size,
                                 int *status)
{
    bool locked = bql_locked();

    *status = -EINVAL;
    if (!directory || size > LOOM_DIRECTORY_CHUNK ||
        offset > sizeof(LoomPCINode) || size > sizeof(LoomPCINode) - offset) {
        return;
    }
    if (!locked) {
        bql_lock();
    }
    memcpy(buffer, (uint8_t *)&directory[local_node] + offset, size);
    *status = 0;
    if (!locked) {
        bql_unlock();
    }
}

static void loom_pci_owner_exit(void)
{
    if (directory) {
        qatomic_store_release(&directory[local_node].ready, 0);
    }
}

void loom_pci_publish(PCIDevice *dev)
{
    LoomPCIDescriptor *d;
    unsigned i, bar;
    uint64_t sequence;

    if (!directory || !dev->loom_aggregate) {
        return;
    }
    for (i = 0; i < owned_count && owned[i] != dev; i++) {
    }
    if (i == owned_count) {
        return;
    }
    if (owned_reset_depth[i]) {
        return;
    }
    d = &directory[local_node].devices[i];
    sequence = qatomic_read(&d->sequence) | 1;
    qatomic_set(&d->sequence, sequence);
    smp_wmb();
    d->generation = owned_generation[i];
    d->bdf = (pci_dev_bus_num(dev) << 8) | dev->devfn;
    d->config_size = pci_config_size(dev);
    d->numa_node = dev->loom_numa_node >= 0 ?
                   dev->loom_numa_node : local_node;
    d->cap_present = dev->cap_present;
    memcpy(d->config, dev->config, d->config_size);
    for (bar = 0; bar < PCI_NUM_REGIONS; bar++) {
        d->bars[bar].address = dev->io_regions[bar].addr;
        d->bars[bar].size = dev->io_regions[bar].size;
        d->bars[bar].type = dev->io_regions[bar].type;
    }
    qatomic_store_release(&d->sequence, sequence + 1);
    publish_directory_update(i);
}

static void collect_device(PCIBus *bus, PCIDevice *dev, void *opaque)
{
    if (!dev->loom_aggregate) {
        return;
    }
    if (bus != root_bus ||
        (dev->config[PCI_HEADER_TYPE] & 0x7f) != PCI_HEADER_TYPE_NORMAL) {
        fatal_error("initial implementation requires endpoints on the root bus");
    }
    if (owned_count == LOOM_MAX_DEVICES) {
        fatal_error("too many owned PCI devices");
    }
    owned_generation[owned_count] = 1;
    owned[owned_count++] = dev;
    loom_pci_publish(dev);
}

void loom_pci_prepare(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    PCIHostState *host;

    if (!loom_pci_enabled()) {
        return;
    }
    if (ms->local_cpus <= 0 || ms->qemu_nums < 2 ||
        ms->qemu_nums > LOOM_MAX_NODES) {
        fatal_error("requires between two and four nodes with explicit CPU ownership");
    }
    local_node = ms->local_cpu_start_index / ms->local_cpus;
    node_count = ms->qemu_nums;
    if (local_node >= node_count) {
        fatal_error("invalid local node");
    }
    host = QLIST_FIRST(&pci_host_bridges);
    if (!host || QLIST_NEXT(host, next) || pci_bus_num(host->bus) != 0) {
        fatal_error("initial implementation requires one PCI root bus numbered zero");
    }
    root_bus = host->bus;
    qemu_mutex_init(&directory_lock);
    qemu_cond_init(&directory_changed);
    directory = g_new0(LoomPCINode, node_count);
    incoming_directory = g_new0(LoomPCINode, node_count);
    directory[local_node].magic = LOOM_DIRECTORY_MAGIC;
    directory[local_node].version = LOOM_DIRECTORY_VERSION;
    pci_for_each_device_under_bus(root_bus, collect_device, NULL);
    directory[local_node].count = owned_count;
    directory[local_node].heartbeat = 1;
    qatomic_store_release(&directory[local_node].ready, 1);
    atexit(loom_pci_owner_exit);
}

static void execute(unsigned owner, LoomPCIRequest *req, LoomPCIResponse *resp)
{
    if (owner == local_node) {
        loom_pci_dispatch(req, resp);
    } else {
        int status = loom_pci_rpc(owner, req, resp);
        if (status < 0) {
            error_report("Loom PCI owner=%u device=%u op=%u bar=%u "
                         "offset=0x%" PRIx64 " size=%u status=%d",
                         owner, req->device_id, req->operation, req->bar,
                         req->offset, req->size, status);
            fatal_error("owner request failed; outcome unknown, no automatic retry");
        }
    }
    if (resp->status || (resp->generation != req->generation &&
                         req->operation != LOOM_PCI_CONFIG_WRITE)) {
        error_report("Loom PCI: rejected owner=%u device=%u operation=%u "
                     "bar=%u offset=0x%" PRIx64 " status=%d "
                     "requested_generation=%u returned_generation=%u",
                     owner, req->device_id, req->operation, req->bar,
                     req->offset, resp->status, req->generation,
                     resp->generation);
        fatal_error("owner rejected device access");
    }
}

static uint32_t remote_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    LoomRemotePCI *s = (LoomRemotePCI *)dev;
    LoomPCIDescriptor current;
    snapshot(descriptor(s->device_id), &current);
    s->generation = current.generation;
    LoomPCIRequest req = {
        .operation = LOOM_PCI_CONFIG_READ,
        .device_id = s->device_id, .generation = s->generation,
        .offset = addr, .size = len,
    };
    LoomPCIResponse resp;

    execute(s->device_id / LOOM_MAX_DEVICES, &req, &resp);
    return resp.value;
}

static void remote_config_write(PCIDevice *dev, uint32_t addr,
                                 uint32_t value, int len)
{
    LoomRemotePCI *s = (LoomRemotePCI *)dev;
    LoomPCIDescriptor current;
    snapshot(descriptor(s->device_id), &current);
    s->generation = current.generation;
    LoomPCIRequest req = {
        .operation = LOOM_PCI_CONFIG_WRITE,
        .device_id = s->device_id, .generation = s->generation,
        .offset = addr, .size = len, .value = value,
    };
    LoomPCIResponse resp;
    LoomPCIDescriptor d;

    execute(s->device_id / LOOM_MAX_DEVICES, &req, &resp);
    snapshot(descriptor(s->device_id), &d);
    /* The proxy has no hardware side effects or writable configuration. */
    memcpy(dev->config, d.config, d.config_size);
    pci_update_mappings(dev);
}

static uint64_t remote_bar_read(void *opaque, hwaddr offset, unsigned size)
{
    typeof(((LoomRemotePCI *)0)->bar_context[0]) *ctx = opaque;
    LoomRemotePCI *s = ctx->device;
    LoomPCIDescriptor current;
    snapshot(descriptor(s->device_id), &current);
    if (current.bars[ctx->bar].address == PCI_BAR_UNMAPPED ||
        current.bars[ctx->bar].address !=
        s->parent_obj.io_regions[ctx->bar].addr) {
        return UINT64_MAX;
    }
    s->generation = current.generation;
    LoomPCIRequest req = {
        .operation = LOOM_PCI_BAR_READ,
        .device_id = s->device_id, .generation = s->generation,
        .bar = ctx->bar, .offset = offset, .size = size,
    };
    LoomPCIResponse resp;
    execute(s->device_id / LOOM_MAX_DEVICES, &req, &resp);
    return resp.value;
}

static void remote_bar_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    typeof(((LoomRemotePCI *)0)->bar_context[0]) *ctx = opaque;
    LoomRemotePCI *s = ctx->device;
    LoomPCIDescriptor current;
    snapshot(descriptor(s->device_id), &current);
    if (current.bars[ctx->bar].address == PCI_BAR_UNMAPPED ||
        current.bars[ctx->bar].address !=
        s->parent_obj.io_regions[ctx->bar].addr) {
        return;
    }
    s->generation = current.generation;
    LoomPCIRequest req = {
        .operation = LOOM_PCI_BAR_WRITE,
        .device_id = s->device_id, .generation = s->generation,
        .bar = ctx->bar, .offset = offset, .size = size, .value = value,
    };
    LoomPCIResponse resp;
    execute(s->device_id / LOOM_MAX_DEVICES, &req, &resp);
}

static const MemoryRegionOps remote_bar_ops = {
    .read = remote_bar_read,
    .write = remote_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void remote_realize(PCIDevice *dev, Error **errp)
{
    LoomRemotePCI *s = (LoomRemotePCI *)dev;
    LoomPCIDescriptor d;
    unsigned bar;
    snapshot(descriptor(s->device_id), &d);
    memcpy(dev->config, d.config, d.config_size);
    memset(dev->wmask, 0, d.config_size);
    memset(dev->w1cmask, 0, d.config_size);
    for (bar = 0; bar < PCI_NUM_REGIONS; bar++) {
        if (!d.bars[bar].size) {
            continue;
        }
        s->bar_context[bar].device = s;
        s->bar_context[bar].bar = bar;
        memory_region_init_io(&s->bars[bar], OBJECT(dev), &remote_bar_ops,
                              &s->bar_context[bar], "loom-remote-bar",
                              d.bars[bar].size);
        pci_register_bar(dev, bar, d.bars[bar].type, &s->bars[bar]);
    }
    memcpy(dev->config, d.config, d.config_size);
}

static void remote_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = remote_realize;
    pc->config_read = remote_config_read;
    pc->config_write = remote_config_write;
}

static const TypeInfo remote_info = {
    .name = TYPE_LOOM_REMOTE_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(LoomRemotePCI),
    .class_init = remote_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void register_types(void)
{
    type_register_static(&remote_info);
}
type_init(register_types)

void loom_pci_bootstrap(void)
{
    int64_t deadline;
    unsigned owner, index, bdf;
    int ids[256];

    if (!directory) {
        return;
    }
    deadline = g_get_monotonic_time() + 30000000;
    for (owner = 0; owner < node_count; owner++) {
        directory_read(owner, 0, &directory[owner],
                       offsetof(LoomPCINode, devices));
        while (!qatomic_load_acquire(&directory[owner].ready)) {
            if (g_get_monotonic_time() >= deadline) {
                fatal_error("device directory startup barrier timed out");
            }
            g_usleep(1000);
            directory_read(owner, 0, &directory[owner],
                           offsetof(LoomPCINode, devices));
        }
        if (directory[owner].magic != LOOM_DIRECTORY_MAGIC ||
            directory[owner].version != LOOM_DIRECTORY_VERSION ||
            directory[owner].count > LOOM_MAX_DEVICES) {
            fatal_error("incompatible device directory");
        }
    }
    memset(ids, -1, sizeof(ids));
    for (owner = 0; owner < node_count; owner++) {
        for (index = 0; index < directory[owner].count; index++) {
            LoomPCIDescriptor *d = &directory[owner].devices[index];
            if (owner != local_node) {
                uint64_t after;
                int64_t limit = g_get_monotonic_time() + 5000000;
                unsigned offset = offsetof(LoomPCINode, devices) +
                                  index * sizeof(*d);
                do {
                    if (g_get_monotonic_time() >= limit) {
                        fatal_error("unstable owner descriptor");
                    }
                    directory_read(owner, offset, d, sizeof(*d));
                    directory_read(owner, offset, &after, sizeof(after));
                } while (d->sequence != after || (after & 1));
            }
            bdf = d->bdf;
            if (bdf >= ARRAY_SIZE(ids) || ids[bdf] != -1) {
                fatal_error("duplicate or unsupported global BDF");
            }
            if (d->config_size != PCI_CONFIG_SPACE_SIZE &&
                d->config_size != PCIE_CONFIG_SPACE_SIZE) {
                fatal_error("invalid PCI configuration size");
            }
            ids[bdf] = owner * LOOM_MAX_DEVICES + index;
            if (owner != local_node) {
                qemu_mutex_lock(&directory_lock);
                LoomPCIDescriptor *cached =
                    &incoming_directory[owner].devices[index];
                if (!incoming_valid[owner][index] ||
                    cached->sequence <= d->sequence) {
                    *cached = *d;
                } else {
                    /*
                     * Preserve a newer control prefix received during the
                     * initial RPC, while filling immutable extended config.
                     */
                    memcpy(cached->config + PCI_CONFIG_SPACE_SIZE,
                           d->config + PCI_CONFIG_SPACE_SIZE,
                           PCIE_CONFIG_SPACE_SIZE - PCI_CONFIG_SPACE_SIZE);
                }
                incoming_valid[owner][index] = true;
                qemu_mutex_unlock(&directory_lock);
            }
        }
    }
    qatomic_store_release(&directory_control_active, true);
    for (index = 0; index < owned_count; index++) {
        publish_directory_update(index);
    }
    for (bdf = 0; bdf < ARRAY_SIZE(ids); bdf++) {
        unsigned id;
        LoomPCIDescriptor *d;
        PCIDevice *dev;
        LoomRemotePCI *remote;
        if (ids[bdf] < 0) {
            continue;
        }
        id = ids[bdf];
        d = descriptor(id);
        owner = id / LOOM_MAX_DEVICES;
        if (owner != local_node) {
            if (root_bus->devices[bdf]) {
                fatal_error("remote BDF conflicts with an existing local device");
            }
            dev = pci_new(bdf, TYPE_LOOM_REMOTE_PCI);
            remote = (LoomRemotePCI *)dev;
            remote->device_id = id;
            remote->generation = d->generation;
            dev->cap_present |= d->cap_present &
                (QEMU_PCI_CAP_EXPRESS | QEMU_PCI_CAP_MULTIFUNCTION);
            pci_realize_and_unref(dev, root_bus, &error_fatal);
        }
        fprintf(stderr, "Loom PCI directory node=%u bdf=00:%02x.%u "
                "owner=%u numa=%d vendor=%04x device=%04x generation=%u\n",
                local_node, PCI_SLOT(bdf), PCI_FUNC(bdf), owner, d->numa_node,
                pci_get_word(d->config + PCI_VENDOR_ID),
                pci_get_word(d->config + PCI_DEVICE_ID), d->generation);
    }
}

void loom_pci_dispatch(const LoomPCIRequest *req, LoomPCIResponse *resp)
{
    unsigned index = req->device_id % LOOM_MAX_DEVICES;
    PCIDevice *dev;
    bool locked = bql_locked();
    uint8_t data[8];
    LoomPCIDescriptor *d = descriptor(req->device_id);
    *resp = (LoomPCIResponse) { .status = -EINVAL };
    if (!d || req->device_id / LOOM_MAX_DEVICES != local_node ||
        index >= owned_count ||
        !(req->size == 1 || req->size == 2 || req->size == 4 || req->size == 8)) {
        return;
    }
    dev = owned[index];
    if (!locked) {
        bql_lock();
    }
    resp->generation = d->generation;
    if (req->generation != d->generation) {
        resp->status = -ESTALE;
        goto out;
    }
    switch (req->operation) {
    case LOOM_PCI_RESET:
    {
        uint32_t bars[6], rom;
        uint16_t command = pci_get_word(dev->config + PCI_COMMAND);

        if (command & PCI_COMMAND_MASTER || dev->irq_state) {
            resp->status = -EBUSY;
            break;
        }
        if (!DEVICE_GET_CLASS(dev)->reset) {
            resp->status = -EOPNOTSUPP;
            break;
        }
        for (unsigned bar = 0; bar < 6; bar++) {
            bars[bar] = pci_get_long(dev->config + PCI_BASE_ADDRESS_0 + bar * 4);
        }
        rom = pci_get_long(dev->config + PCI_ROM_ADDRESS);
        loom_pci_reset_begin(dev);
        pci_device_reset(dev);
        /* Preserve Linux's already allocated BAR resources. Guest software
         * must first unbind/quiesce the driver; queue state is not restored.
         */
        for (unsigned bar = 0; bar < 6; bar++) {
            pci_host_config_write_common(dev, PCI_BASE_ADDRESS_0 + bar * 4,
                                          pci_config_size(dev), bars[bar], 4);
        }
        pci_host_config_write_common(dev, PCI_ROM_ADDRESS, pci_config_size(dev), rom, 4);
        pci_host_config_write_common(dev, PCI_COMMAND, pci_config_size(dev), command, 2);
        loom_pci_reset_end(dev);
        resp->status = 0;
        break;
    }
    case LOOM_PCI_CONFIG_READ:
    case LOOM_PCI_CONFIG_WRITE:
        if (req->size > 4 || req->offset >= pci_config_size(dev) ||
            req->size > pci_config_size(dev) - req->offset) {
            break;
        }
        if (req->operation == LOOM_PCI_CONFIG_READ) {
            resp->value = pci_host_config_read_common(dev, req->offset,
                                                      pci_config_size(dev),
                                                      req->size);
        } else {
            pci_host_config_write_common(dev, req->offset,
                                          pci_config_size(dev),
                                          req->value, req->size);
        }
        resp->status = 0;
        break;
    case LOOM_PCI_BAR_READ:
    case LOOM_PCI_BAR_WRITE:
        if (req->bar < PCI_NUM_REGIONS) {
            PCIIORegion *bar = &dev->io_regions[req->bar];
            bool write = req->operation == LOOM_PCI_BAR_WRITE;
            if (bar->addr == PCI_BAR_UNMAPPED || req->offset >= bar->size ||
                req->size > bar->size - req->offset) {
                break;
            }
            stq_le_p(data, req->value);
            resp->status = address_space_rw(
                bar->type & PCI_BASE_ADDRESS_SPACE_IO ?
                &address_space_io : &address_space_memory,
                bar->addr + req->offset, MEMTXATTRS_UNSPECIFIED,
                data, req->size, write) == MEMTX_OK ? 0 : -EIO;
            if (!write && !resp->status) {
                resp->value = 0;
                memcpy(&resp->value, data, req->size);
                resp->value = le64_to_cpu(resp->value);
            }
        }
        break;
    default:
        break;
    }
    resp->generation = d->generation;
out:
    if (!locked) {
        bql_unlock();
    }
}

static void refresh_remote_mappings(void)
{
    unsigned owner, index;
    bool locked = bql_locked();
    uint64_t epoch;
    if (!locked) {
        bql_lock();
    }
    epoch = qatomic_read(&directory_epoch);
    if (qatomic_load_acquire(&directory_control_active) &&
        epoch == applied_directory_epoch) {
        if (!locked) {
            bql_unlock();
        }
        return;
    }
    for (owner = 0; owner < node_count; owner++) {
        if (owner == local_node) {
            continue;
        }
        for (index = 0; index < directory[owner].count; index++) {
            LoomPCIDescriptor d;
            PCIDevice *dev;
            snapshot(&directory[owner].devices[index], &d);
            dev = root_bus->devices[d.bdf];
            if (dev && memcmp(dev->config, d.config, d.config_size)) {
                memcpy(dev->config, d.config, d.config_size);
                pci_update_mappings(dev);
            }
        }
    }
    applied_directory_epoch = epoch;
    if (!locked) {
        bql_unlock();
    }
}

static bool bar_access_owner(unsigned owner, hwaddr addr, bool io,
                             uint8_t *data, unsigned size, bool write)
{
    unsigned index, bar;

    for (index = 0; index < directory[owner].count; index++) {
        LoomPCIDescriptor d;
        snapshot(&directory[owner].devices[index], &d);
        for (bar = 0; bar < PCI_NUM_REGIONS; bar++) {
            LoomBAR *b = &d.bars[bar];
            LoomPCIRequest req;
            LoomPCIResponse resp;
            if (!b->size || b->address == PCI_BAR_UNMAPPED ||
                !!(b->type & PCI_BASE_ADDRESS_SPACE_IO) != io ||
                addr < b->address || addr - b->address >= b->size) {
                continue;
            }
            req = (LoomPCIRequest) {
                .operation = write ? LOOM_PCI_BAR_WRITE : LOOM_PCI_BAR_READ,
                .device_id = owner * LOOM_MAX_DEVICES + index,
                .generation = d.generation, .bar = bar,
                .offset = addr - b->address, .size = size,
            };
            if (write) {
                memcpy(&req.value, data, size);
                req.value = le64_to_cpu(req.value);
            }
            execute(owner, &req, &resp);
            if (!write) {
                uint64_t value = cpu_to_le64(resp.value);
                memcpy(data, &value, size);
            }
            return true;
        }
    }
    return false;
}

static bool bar_access(hwaddr addr, bool io, uint8_t *data,
                        unsigned size, bool write)
{
    unsigned owner;

    if (!directory) {
        return false;
    }
    if (!(size == 1 || size == 2 || size == 4 || size == 8)) {
        fatal_error("unsupported BAR access width");
    }
    /*
     * Owner-local descriptors are authoritative and published under the BQL.
     * Local access still validates the generation in loom_pci_dispatch(), but
     * must not wait for an unrelated peer's configuration snapshot.
     */
    if (bar_access_owner(local_node, addr, io, data, size, write)) {
        return true;
    }

    /*
     * Owner control operations publish and acknowledge new prefixes.
     * Apply changed proxy mappings locally; no directory RPC on data access.
     */
    refresh_remote_mappings();
    for (owner = 0; owner < node_count; owner++) {
        if (owner != local_node &&
            bar_access_owner(owner, addr, io, data, size, write)) {
            return true;
        }
    }
    return false;
}

void loom_pci_config_address(uint32_t value)
{
    qatomic_set(&config_address, value);
}

bool loom_pci_handle_pio(uint16_t port, MemTxAttrs attrs, void *data,
                         int direction, int size, uint32_t count)
{
    uint8_t *bytes = data;
    unsigned i, owner, index;
    bool configuration = false;
    uint32_t address = qatomic_read(&config_address);
    if (!directory) {
        return false;
    }
    if (port >= 0xcf8 && port < 0xcfc) {
        for (i = 0; i < count; i++) {
            address_space_rw(&address_space_io, port, attrs, bytes + i * size,
                             size, direction == KVM_EXIT_IO_OUT);
        }
        if (direction == KVM_EXIT_IO_OUT) {
            pio_forwarding(port, attrs, data, direction, size, count, true);
        }
        return true;
    }
    if (port >= 0xcfc && port < 0xd00 && (address & (1U << 31))) {
        uint16_t bdf = address >> 8;
        for (owner = 0; owner < node_count; owner++) {
            for (index = 0; index < directory[owner].count; index++) {
                configuration |= directory[owner].devices[index].bdf == bdf;
            }
        }
        if (configuration) {
            for (i = 0; i < count; i++) {
                address_space_rw(&address_space_io, port, attrs,
                                  bytes + i * size, size,
                                  direction == KVM_EXIT_IO_OUT);
            }
            return true;
        }
    }
    for (i = 0; i < count; i++) {
        if (!bar_access(port, true, bytes + i * size, size,
                         direction == KVM_EXIT_IO_OUT)) {
            return false;
        }
    }
    return true;
}

bool loom_pci_handle_mmio(hwaddr addr, MemTxAttrs attrs, uint8_t *data,
                          unsigned size, bool write)
{
    return bar_access(addr, false, data, size, write);
}
