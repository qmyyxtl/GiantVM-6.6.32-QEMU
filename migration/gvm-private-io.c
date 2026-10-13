/*
 * Request-scoped private-buffer RDMA transport for distributed virtio-blk.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A prototype two-sided bulk transport: registered private SEND/RECV buffers
 * on dedicated RC QPs/CQs/threads. No guest page is registered for remote DMA.
 * NUMA is a routing hint, not proof of DSM ownership: the memory home always
 * uses KVM_DSM_MEMCPY to acquire coherent access before copying.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "qemu/memalign.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "sysemu/hostmem.h"
#include "sysemu/kvm.h"
#include "migration/blocker.h"
#include "gvm-private-io.h"
#include <linux/kvm.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <poll.h>

#define IO_NODES 4
#define IO_BYTES (2 * 1024 * 1024)
#define IO_HEADER 4096
#define IO_BUFFER (IO_HEADER + IO_BYTES)
#define IO_MAGIC 0x4756494f
#define IO_VERSION 1
#define IO_READ 1
#define IO_WRITE 2
#define IO_ECHO 3
#define IO_POOL_LIMIT (64 * 1024 * 1024)

typedef struct IOHeader {
    uint32_t magic, version, operation, length;
    uint64_t sequence, offset;
    int32_t status;
    uint32_t reserved;
} IOHeader;

typedef struct IOConnection {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *events;
    struct ibv_mr *tx_mr, *rx_mr;
    uint8_t *tx, *rx;
    pthread_mutex_t lock;
    uint64_t sequence;
    bool armed;
    int peer;
} IOConnection;

typedef struct IOMapping {
    void *buffer;
    uint64_t offset;
    size_t length;
    int peer;
    bool write;
} IOMapping;

static struct {
    bool initialized, enabled, stopping;
    int local, nodes;
    MemoryRegion *ram[IO_NODES];
    IOConnection *out[IO_NODES], *in[IO_NODES];
    pthread_t threads[IO_NODES];
    pthread_mutex_t maps_lock;
    GHashTable *maps;
    size_t allocated;
    uint64_t calls, read_bytes, write_bytes, remote_maps, local_maps;
    Error *migration_blocker;
} io;

static int64_t io_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Fail closed: no blind retry after an uncertain write or connection failure. */
static G_NORETURN void io_fatal(const char *what)
{
    error_report("GVM private I/O failed: %s (%s); stopping before guest completion",
                 what, strerror(errno));
    _exit(EXIT_FAILURE);
}

static int io_cm_event(struct rdma_event_channel *ch,
                       enum rdma_cm_event_type expected,
                       struct rdma_cm_id **accepted)
{
    struct pollfd p = { .fd = ch->fd, .events = POLLIN };
    struct rdma_cm_event *event;
    if (poll(&p, 1, 30000) != 1 || rdma_get_cm_event(ch, &event)) {
        return -1;
    }
    bool ok = event->event == expected && !event->status;
    if (ok && accepted) {
        *accepted = event->id;
    }
    if (!ok) {
        error_report("private I/O CM: expected %s, got %s status %d",
                     rdma_event_str(expected), rdma_event_str(event->event),
                     event->status);
    }
    rdma_ack_cm_event(event);
    return ok ? 0 : -1;
}

static void io_post_recv(IOConnection *c)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)c->rx, .length = IO_BUFFER,
        .lkey = c->rx_mr->lkey,
    };
    struct ibv_recv_wr wr = { .wr_id = 2, .sg_list = &sge, .num_sge = 1 }, *bad;
    if (ibv_post_recv(c->id->qp, &wr, &bad)) {
        io_fatal("post receive");
    }
}

static void io_post_send(IOConnection *c, size_t length)
{
    struct ibv_sge sge = {
        .addr = (uintptr_t)c->tx, .length = length, .lkey = c->tx_mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id = 1, .sg_list = &sge, .num_sge = 1,
        .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
    }, *bad;
    if (ibv_post_send(c->id->qp, &wr, &bad)) {
        io_fatal("post send");
    }
}

/* CQ belongs to one RPC caller or one server thread, never both. */
static int io_completion(IOConnection *c, struct ibv_wc *wc, bool idle)
{
    int64_t end = io_ms() + 30000;
    for (;;) {
        int n = ibv_poll_cq(c->cq, 1, wc);
        if (n < 0) {
            return -1;
        }
        if (n) {
            return wc->status == IBV_WC_SUCCESS ? 1 : -1;
        }
        if (qatomic_read(&io.stopping)) {
            return 0;
        }
        if (!idle && io_ms() >= end) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (!c->armed) {
            if (ibv_req_notify_cq(c->cq, 0)) {
                return -1;
            }
            c->armed = true;
            continue; /* Recheck after arming to close the lost-wakeup window. */
        }
        struct pollfd p = { .fd = c->events->fd, .events = POLLIN };
        int ret = poll(&p, 1, 100);
        if (ret < 0 && errno != EINTR) {
            return -1;
        }
        if (ret > 0) {
            struct ibv_cq *cq;
            void *ctx;
            if (ibv_get_cq_event(c->events, &cq, &ctx)) {
                return -1;
            }
            ibv_ack_cq_events(cq, 1);
            c->armed = false;
            if (cq != c->cq) {
                return -1;
            }
        }
    }
}

static IOConnection *io_resources(struct rdma_cm_id *id, int peer)
{
    IOConnection *c = g_new0(IOConnection, 1);
    struct ibv_qp_init_attr qp = {
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 4, .max_recv_wr = 4,
                 .max_send_sge = 1, .max_recv_sge = 1 },
    };
    c->id = id;
    c->peer = peer;
    pthread_mutex_init(&c->lock, NULL);
    c->pd = ibv_alloc_pd(id->verbs);
    c->events = ibv_create_comp_channel(id->verbs);
    c->cq = ibv_create_cq(id->verbs, 16, c, c->events, 0);
    if (!c->pd || !c->events || !c->cq) {
        io_fatal("allocate CQ/PD");
    }
    qp.send_cq = qp.recv_cq = c->cq;
    if (rdma_create_qp(id, c->pd, &qp)) {
        io_fatal("create QP");
    }
    c->tx = qemu_memalign(4096, IO_BUFFER);
    c->rx = qemu_memalign(4096, IO_BUFFER);
    memset(c->tx, 0, IO_BUFFER);
    memset(c->rx, 0, IO_BUFFER);
    c->tx_mr = ibv_reg_mr(c->pd, c->tx, IO_BUFFER, IBV_ACCESS_LOCAL_WRITE);
    c->rx_mr = ibv_reg_mr(c->pd, c->rx, IO_BUFFER, IBV_ACCESS_LOCAL_WRITE);
    if (!c->tx_mr || !c->rx_mr) {
        io_fatal("register private memory");
    }
    return c;
}

static IOConnection *io_connect(const char *host, int port, int peer)
{
    struct rdma_event_channel *ch = rdma_create_event_channel();
    struct rdma_cm_id *id;
    struct sockaddr_in addr = {
        .sin_family = AF_INET, .sin_port = htons(port),
    };
    struct rdma_conn_param params = { .retry_count = 7, .rnr_retry_count = 7 };
    if (!ch || rdma_create_id(ch, &id, NULL, RDMA_PS_TCP) ||
        inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        rdma_resolve_addr(id, NULL, (struct sockaddr *)&addr, 5000) ||
        io_cm_event(ch, RDMA_CM_EVENT_ADDR_RESOLVED, NULL) ||
        rdma_resolve_route(id, 5000) ||
        io_cm_event(ch, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL)) {
        io_fatal("resolve RDMA peer");
    }
    IOConnection *c = io_resources(id, peer);
    if (rdma_connect(id, &params) ||
        io_cm_event(ch, RDMA_CM_EVENT_ESTABLISHED, NULL)) {
        io_fatal("connect RDMA peer");
    }
    return c;
}

static IOConnection *io_accept(struct rdma_cm_id *listener, int peer)
{
    struct rdma_cm_id *id;
    struct rdma_conn_param params = { .retry_count = 7, .rnr_retry_count = 7 };
    if (io_cm_event(listener->channel, RDMA_CM_EVENT_CONNECT_REQUEST, &id)) {
        io_fatal("accept RDMA peer");
    }
    IOConnection *c = io_resources(id, peer);
    io_post_recv(c);
    if (rdma_accept(id, &params) ||
        io_cm_event(listener->channel, RDMA_CM_EVENT_ESTABLISHED, NULL)) {
        io_fatal("establish RDMA peer");
    }
    /* Child shares its listener's event channel; retained until shutdown. */
    return c;
}

static int io_guest_copy(uint64_t offset, void *data, size_t length, bool write)
{
    MemoryRegion *mr = io.ram[io.local];
    uint64_t size = memory_region_size(mr);
    if (!length || offset > size || length > size - offset) {
        return -EINVAL;
    }
    struct kvm_dsm_memcpy copy = {
        .write = write,
        .host_virt_addr = (uintptr_t)memory_region_get_ram_ptr(mr) + offset,
        .userspace_addr = (uintptr_t)data, .length = length,
    };
    int ret = kvm_vm_ioctl(kvm_state, KVM_DSM_MEMCPY, &copy);
    if (ret) {
        return ret < 0 ? ret : -EFAULT;
    }
    if (write) {
        memory_region_set_dirty(mr, offset, length);
    }
    return 0;
}

static void *io_service(void *opaque)
{
    IOConnection *c = opaque;
    struct ibv_wc wc, pending;
    bool have_pending = false;
    pthread_setname_np(pthread_self(), "gvm-io-memory");
    rcu_register_thread();
    while (!qatomic_read(&io.stopping)) {
        int ret;
        if (have_pending) {
            wc = pending;
            have_pending = false;
            ret = 1;
        } else {
            ret = io_completion(c, &wc, true);
        }
        if (!ret) {
            break;
        }
        if (ret < 0 || wc.wr_id != 2 || wc.byte_len < IO_HEADER) {
            io_fatal("receive private request");
        }
        IOHeader req = *(IOHeader *)c->rx;
        bool payload = req.operation == IO_WRITE || req.operation == IO_ECHO;
        if (req.magic != IO_MAGIC || req.version != IO_VERSION ||
            !req.length || req.length > IO_BYTES ||
            wc.byte_len != IO_HEADER + (payload ? req.length : 0) ||
            req.sequence != ++c->sequence || req.status || req.reserved) {
            io_fatal("invalid private request");
        }
        IOHeader *resp = (IOHeader *)c->tx;
        *resp = req;
        size_t bytes = IO_HEADER;
        switch (req.operation) {
        case IO_WRITE:
            resp->status = io_guest_copy(req.offset, c->rx + IO_HEADER,
                                         req.length, true);
            break;
        case IO_READ:
            resp->status = io_guest_copy(req.offset, c->tx + IO_HEADER,
                                         req.length, false);
            if (!resp->status) {
                bytes += req.length;
            }
            break;
        case IO_ECHO:
            memcpy(c->tx + IO_HEADER, c->rx + IO_HEADER, req.length);
            bytes += req.length;
            break;
        default:
            io_fatal("unknown private operation");
        }
        /* Replenish before replying: otherwise the next request can hit
         * RC receiver-not-ready retry, whose default delay is very large.
         * The next receive completion may precede our send completion.
         */
        io_post_recv(c);
        io_post_send(c, bytes);
        for (;;) {
            if (io_completion(c, &wc, false) != 1) {
                io_fatal("send private response");
            }
            if (wc.wr_id == 1) {
                break;
            }
            if (wc.wr_id != 2 || have_pending) {
                io_fatal("unexpected private receive completion");
            }
            pending = wc;
            have_pending = true;
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static void io_call(int peer, unsigned operation, uint64_t offset,
                     void *buffer, size_t length)
{
    IOConnection *c = io.out[peer];
    struct ibv_wc wc;
    pthread_mutex_lock(&c->lock);
    IOHeader *req = (IOHeader *)c->tx;
    *req = (IOHeader) {
        .magic = IO_MAGIC, .version = IO_VERSION, .operation = operation,
        .length = length, .sequence = ++c->sequence, .offset = offset,
    };
    bool payload = operation == IO_WRITE || operation == IO_ECHO;
    if (payload) {
        memcpy(c->tx + IO_HEADER, buffer, length);
    }
    io_post_recv(c);
    io_post_send(c, IO_HEADER + (payload ? length : 0));
    unsigned seen = 0, received = 0;
    while (seen != 3) {
        if (io_completion(c, &wc, false) != 1 ||
            (wc.wr_id != 1 && wc.wr_id != 2) ||
            (seen & (unsigned)wc.wr_id)) {
            io_fatal("private RPC completion");
        }
        seen |= wc.wr_id;
        if (wc.wr_id == 2) {
            received = wc.byte_len;
        }
    }
    IOHeader *resp = (IOHeader *)c->rx;
    bool result_data = operation == IO_READ || operation == IO_ECHO;
    if (received < IO_HEADER || resp->magic != IO_MAGIC ||
        resp->version != IO_VERSION || resp->sequence != req->sequence ||
        resp->operation != operation || resp->offset != offset ||
        resp->length != length || resp->status ||
        received != IO_HEADER + (result_data ? length : 0)) {
        io_fatal("private RPC response");
    }
    if (result_data) {
        memcpy(buffer, c->rx + IO_HEADER, length);
    }
    qatomic_inc(&io.calls);
    if (operation == IO_READ) {
        qatomic_add(&io.read_bytes, length);
    } else if (operation == IO_WRITE) {
        qatomic_add(&io.write_bytes, length);
    }
    pthread_mutex_unlock(&c->lock);
}

static bool io_get_enabled(Object *obj, Error **errp)
{
    return qatomic_read(&io.enabled);
}

static void io_get_stats(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    g_autofree char *stats = g_strdup_printf(
        "calls=%" PRIu64 " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
        " remote_maps=%" PRIu64 " local_maps=%" PRIu64,
        qatomic_read(&io.calls), qatomic_read(&io.read_bytes),
        qatomic_read(&io.write_bytes), qatomic_read(&io.remote_maps),
        qatomic_read(&io.local_maps));
    visit_type_str(v, name, &stats, errp);
}

void gvm_private_io_start(int local, int nodes, char **hosts)
{
    if (g_strcmp0(getenv("GVM_PRIVATE_IO"), "1")) {
        return;
    }
    MachineState *ms = MACHINE(qdev_get_machine());
    if (nodes < 2 || nodes > IO_NODES || !ms->numa_state ||
        ms->numa_state->num_nodes != nodes || ms->shm_path) {
        io_fatal("requires one RAM memdev per distributed NUMA node");
    }
    io.local = local;
    io.nodes = nodes;
    io.maps = g_hash_table_new(g_direct_hash, g_direct_equal);
    pthread_mutex_init(&io.maps_lock, NULL);
    for (int i = 0; i < nodes; i++) {
        HostMemoryBackend *backend = ms->numa_state->nodes[i].node_memdev;
        if (!backend) {
            io_fatal("NUMA memdev missing");
        }
        io.ram[i] = host_memory_backend_get_memory(backend);
        memory_region_ref(io.ram[i]);
    }

    struct rdma_cm_id *listeners[IO_NODES] = { 0 };
    for (int peer = 0; peer < nodes; peer++) {
        if (peer == local) {
            continue;
        }
        struct rdma_event_channel *ch = rdma_create_event_channel();
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(41000 + local * IO_NODES + peer),
        };
        if (!ch || rdma_create_id(ch, &listeners[peer], NULL, RDMA_PS_TCP) ||
            inet_pton(AF_INET, hosts[local], &addr.sin_addr) != 1 ||
            rdma_bind_addr(listeners[peer], (struct sockaddr *)&addr) ||
            rdma_listen(listeners[peer], 1)) {
            io_fatal("listen private RDMA");
        }
    }
    for (int peer = 0; peer < nodes; peer++) {
        if (peer == local) {
            continue;
        }
        int port = 41000 + peer * IO_NODES + local;
        if (local < peer) {
            io.out[peer] = io_connect(hosts[peer], port, peer);
            io.in[peer] = io_accept(listeners[peer], peer);
        } else {
            io.in[peer] = io_accept(listeners[peer], peer);
            io.out[peer] = io_connect(hosts[peer], port, peer);
        }
        rdma_destroy_id(listeners[peer]);
        if (pthread_create(&io.threads[peer], NULL, io_service, io.in[peer])) {
            io_fatal("start private memory worker");
        }
    }
    /* No guest pages touched by the transport self-test. */
    uint8_t *test = g_malloc(IO_BYTES);
    for (int peer = 0; peer < nodes; peer++) {
        if (peer == local) {
            continue;
        }
        for (unsigned i = 0; i < IO_BYTES; i++) {
            test[i] = (i * 131 + local) & 255;
        }
        io_call(peer, IO_ECHO, 0, test, IO_BYTES);
        for (unsigned i = 0; i < IO_BYTES; i++) {
            if (test[i] != ((i * 131 + local) & 255)) {
                io_fatal("private RDMA echo mismatch");
            }
        }
    }
    g_free(test);
    error_setg(&io.migration_blocker,
               "GVM private I/O prototype has no migration/reconnect protocol");
    if (migrate_add_blocker(&io.migration_blocker, &error_fatal)) {
        io_fatal("install migration blocker");
    }
    object_property_add_bool(OBJECT(ms), "gvm-private-io",
                             io_get_enabled, NULL);
    object_property_add(OBJECT(ms), "gvm-private-io-stats", "str",
                        io_get_stats, NULL, NULL, NULL);
    io.initialized = true;
    qatomic_set(&io.enabled, true);
    error_report("GVM private I/O: node %d, independent RDMA pool echo PASS (2 MiB)",
                 local);
}

void gvm_private_io_stop(void)
{
    if (!io.initialized) {
        return;
    }
    qatomic_set(&io.enabled, false);
    qatomic_set(&io.stopping, true);
    for (int peer = 0; peer < io.nodes; peer++) {
        if (peer != io.local) {
            pthread_join(io.threads[peer], NULL);
        }
    }
    /* VM shutdown owns the remaining outstanding slots and RDMA resources.
     * Keep them live until process exit; never recycle uncertain DMA buffers.
     */
}

bool gvm_private_io_map(AddressSpace *as, hwaddr addr, hwaddr *len,
                        bool write, void **buffer)
{
    if (!qatomic_read(&io.enabled)) {
        return false;
    }
    RCU_READ_LOCK_GUARD();
    hwaddr offset, length = *len;
    MemoryRegion *mr = address_space_translate(as, addr, &offset, &length,
                                               write, MEMTXATTRS_UNSPECIFIED);
    int peer;
    for (peer = 0; peer < io.nodes; peer++) {
        if (mr == io.ram[peer]) {
            break;
        }
    }
    if (peer == io.nodes || peer == io.local) {
        qatomic_inc(&io.local_maps);
        return false;
    }
    length = MIN(length, IO_BYTES);
    if (!length || !memory_access_is_direct(mr, write)) {
        *buffer = NULL;
        return true;
    }
    IOMapping *m = g_new0(IOMapping, 1);
    m->length = length;
    m->offset = offset;
    m->peer = peer;
    m->write = write;
    pthread_mutex_lock(&io.maps_lock);
    if (length > IO_POOL_LIMIT - io.allocated) {
        pthread_mutex_unlock(&io.maps_lock);
        g_free(m);
        *buffer = NULL;
        return true;
    }
    io.allocated += length;
    pthread_mutex_unlock(&io.maps_lock);
    m->buffer = qemu_memalign(4096, length);
    if (!write) {
        io_call(peer, IO_READ, offset, m->buffer, length);
    } else {
        /* Do not expose uninitialized host data on a short backend read. */
        memset(m->buffer, 0, length);
    }
    pthread_mutex_lock(&io.maps_lock);
    g_hash_table_insert(io.maps, m->buffer, m);
    pthread_mutex_unlock(&io.maps_lock);
    qatomic_inc(&io.remote_maps);
    *buffer = m->buffer;
    *len = length;
    return true;
}

bool gvm_private_io_unmap(void *buffer, hwaddr accessed, bool write)
{
    if (!io.initialized) {
        return false;
    }
    pthread_mutex_lock(&io.maps_lock);
    IOMapping *m = g_hash_table_lookup(io.maps, buffer);
    if (m) {
        g_hash_table_remove(io.maps, buffer);
    }
    pthread_mutex_unlock(&io.maps_lock);
    if (!m) {
        return false;
    }
    if (m->write != write || accessed > m->length) {
        io_fatal("private mapping direction/length mismatch");
    }
    if (write && accessed) {
        io_call(m->peer, IO_WRITE, m->offset, m->buffer, accessed);
    }
    pthread_mutex_lock(&io.maps_lock);
    io.allocated -= m->length;
    pthread_mutex_unlock(&io.maps_lock);
    qemu_vfree(m->buffer);
    g_free(m);
    return true;
}
