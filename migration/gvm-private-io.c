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
#include "qemu/bitmap.h"
#include "qemu/rcu.h"
#include "qemu/memalign.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/boards.h"
#include "hw/virtio/virtio.h"
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
#define IO_VERSION 3
#define IO_READ 1
#define IO_WRITE 2
#define IO_ECHO 3
#define IO_READV 4
#define IO_WRITEV 5
#define IO_WRITE_POOL 6
#define IO_PAGE 4096
#define IO_POOL_PAGES (IO_POOL_LIMIT / IO_PAGE)
#define IO_SEGMENTS 240
#define IO_POOL_LIMIT (64 * 1024 * 1024)

typedef struct IOHeader {
    uint32_t magic, version, operation, length;
    uint64_t sequence, offset;
    int32_t status;
    uint32_t reserved;
} IOHeader;

typedef struct IOVec {
    uint64_t offset;
    uint32_t length, reserved;
} IOVec;

typedef struct IORemote {
    uint64_t address;
    uint32_t rkey, mode;
} IORemote;

typedef struct IOConnection {
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *events;
    struct ibv_mr *tx_mr, *rx_mr, *pool_mr;
    IORemote remote_pool;
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
    bool write, published;
} IOMapping;

static struct {
    bool initialized, enabled, stopping;
    unsigned mode; /* 0: copies, 1: registered pool, 2: DSM-authorized direct */
    uint8_t *pool;
    unsigned long *pool_used;
    uint64_t send_zcopy_bytes;
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
        .cap = { .max_send_wr = IO_SEGMENTS + 2, .max_recv_wr = 4,
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
    c->rx_mr = ibv_reg_mr(c->pd, c->rx, IO_BUFFER,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (io.mode) {
        c->pool_mr = ibv_reg_mr(c->pd, io.pool, IO_POOL_LIMIT, 0);
        if (!c->pool_mr) {
            io_fatal("register disk/RDMA shared pool");
        }
    }
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
        bool vector = req.operation == IO_READV || req.operation == IO_WRITEV ||
                      req.operation == IO_WRITE_POOL;
        bool payload = req.operation == IO_WRITE || req.operation == IO_ECHO ||
                       req.operation == IO_WRITEV;
        if (req.magic != IO_MAGIC || req.version != IO_VERSION ||
            !req.length || req.length > IO_BYTES ||
            wc.byte_len != IO_HEADER + (payload ? req.length : 0) ||
            req.sequence != ++c->sequence || req.status ||
            (vector ? (!req.reserved || req.reserved > IO_SEGMENTS) :
                      req.reserved != 0)) {
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
        case IO_READV:
        case IO_WRITEV:
        case IO_WRITE_POOL: {
            IOVec *vec = (IOVec *)(c->rx + sizeof(IOHeader));
            unsigned total = 0;
            uint64_t ram_size = memory_region_size(io.ram[io.local]);
            for (unsigned i = 0; i < req.reserved; i++) {
                if (vec[i].reserved || !vec[i].length ||
                    vec[i].length > req.length - total ||
                    vec[i].offset > ram_size ||
                    vec[i].length > ram_size - vec[i].offset) {
                    io_fatal("invalid private vector");
                }
                total += vec[i].length;
            }
            if (total != req.length || req.offset) {
                io_fatal("invalid private vector length");
            }
            bool write = req.operation != IO_READV;
            if (req.operation == IO_WRITE_POOL && !io.mode) {
                io_fatal("unnegotiated pool write");
            }
            uint8_t *data = (write ? c->rx : c->tx) + IO_HEADER;
            total = 0;
            for (unsigned i = 0; i < req.reserved; ) {
                uint64_t offset = vec[i].offset;
                unsigned length = vec[i++].length;
                while (i < req.reserved && vec[i].offset == offset + length) {
                    length += vec[i++].length;
                }
                resp->status = io_guest_copy(offset, data + total, length, write);
                if (resp->status) {
                    break;
                }
                total += length;
            }
            if (!write && !resp->status) {
                bytes += req.length;
            }
            break;
        }
        case IO_ECHO:
            *(IORemote *)(c->tx + sizeof(IOHeader)) = (IORemote) {
                .address = (uintptr_t)c->rx + IO_HEADER,
                .rkey = c->rx_mr->rkey, .mode = io.mode,
            };
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
    if (operation == IO_ECHO) {
        c->remote_pool = *(IORemote *)(c->rx + sizeof(IOHeader));
        if (c->remote_pool.mode != io.mode) {
            io_fatal("private I/O mode mismatch");
        }
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



/* Unsigned data WRs precede a signaled SEND on the same RC QP. The SEND
 * completion retires the entire chain; the peer processes it only after
 * data visibility. No relaxed-ordering MR or cross-QP dependency is used. */
static void io_post_pool_write(IOConnection *c, IOMapping **maps,
                                uint32_t *lengths, unsigned count,
                                uint64_t base, uint32_t rkey, bool guest)
{
    struct ibv_sge sge[IO_SEGMENTS];
    struct ibv_send_wr wr[IO_SEGMENTS], *bad;
    uint64_t offset = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t remote = base + (guest ? maps[i]->offset : offset);
        uintptr_t local = (uintptr_t)maps[i]->buffer;
        if (n && sge[n - 1].addr + sge[n - 1].length == local &&
            wr[n - 1].wr.rdma.remote_addr + sge[n - 1].length == remote) {
            sge[n - 1].length += lengths[i];
        } else {
            sge[n] = (struct ibv_sge) {
                .addr = local, .length = lengths[i], .lkey = c->pool_mr->lkey,
            };
            wr[n] = (struct ibv_send_wr) {
                .wr_id = 3, .sg_list = &sge[n], .num_sge = 1,
                .opcode = IBV_WR_RDMA_WRITE,
                .wr.rdma = { .remote_addr = remote, .rkey = rkey },
            };
            if (n) {
                wr[n - 1].next = &wr[n];
            }
            n++;
        }
        offset += lengths[i];
    }
    if (ibv_post_send(c->id->qp, wr, &bad)) {
        io_fatal("post private pool data");
    }
    qatomic_add(&io.send_zcopy_bytes, offset);
}

/* Pack the scatter/gather list into one registered private RDMA buffer.
 * Individual guest pages never appear in an RDMA MR or remote address.
 */
static void io_callv(int peer, bool write, IOMapping **maps,
                      uint32_t *lengths, unsigned count)
{
    IOConnection *c = io.out[peer];
    struct ibv_wc wc;
    unsigned length = 0;
    pthread_mutex_lock(&c->lock);
    IOHeader *req = (IOHeader *)c->tx;
    IOVec *vec = (IOVec *)(c->tx + sizeof(*req));
    for (unsigned i = 0; i < count; i++) {
        vec[i] = (IOVec) { .offset = maps[i]->offset, .length = lengths[i] };
        if (write && !io.mode) {
            memcpy(c->tx + IO_HEADER + length, maps[i]->buffer, lengths[i]);
        }
        length += lengths[i];
    }
    assert(count && count <= IO_SEGMENTS && length && length <= IO_BYTES);
    *req = (IOHeader) {
        .magic = IO_MAGIC, .version = IO_VERSION,
        .operation = write ? (io.mode ? IO_WRITE_POOL : IO_WRITEV) : IO_READV,
        .length = length, .sequence = ++c->sequence, .reserved = count,
    };
    io_post_recv(c);
    if (write && io.mode) {
        io_post_pool_write(c, maps, lengths, count, c->remote_pool.address,
                           c->remote_pool.rkey, false);
    }
    io_post_send(c, IO_HEADER + (write && !io.mode ? length : 0));
    unsigned seen = 0, received = 0;
    while (seen != 3) {
        if (io_completion(c, &wc, false) != 1 ||
            (wc.wr_id != 1 && wc.wr_id != 2) ||
            (seen & (unsigned)wc.wr_id)) {
            io_fatal("private vector RPC completion");
        }
        seen |= wc.wr_id;
        if (wc.wr_id == 2) {
            received = wc.byte_len;
        }
    }
    IOHeader *resp = (IOHeader *)c->rx;
    if (received < IO_HEADER || resp->magic != IO_MAGIC ||
        resp->version != IO_VERSION || resp->sequence != req->sequence ||
        resp->operation != req->operation || resp->offset ||
        resp->length != length || resp->reserved != count || resp->status ||
        received != IO_HEADER + (write ? 0 : length)) {
        io_fatal("private vector RPC response");
    }
    unsigned offset = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!write) {
            memcpy(maps[i]->buffer, c->rx + IO_HEADER + offset, lengths[i]);
        } else {
            maps[i]->published = true;
        }
        offset += lengths[i];
    }
    qatomic_inc(&io.calls);
    if (write) {
        qatomic_add(&io.write_bytes, length);
    } else {
        qatomic_add(&io.read_bytes, length);
    }
    pthread_mutex_unlock(&c->lock);
}

static void io_element_transfer(const VirtQueueElement *elem,
                                 unsigned accessed, bool write)
{
    if (!io.initialized || !qatomic_read(&io.allocated)) {
        return;
    }
    unsigned num = write ? elem->in_num : elem->out_num;
    struct iovec *sg = write ? elem->in_sg : elem->out_sg;
    IOMapping *maps[IO_SEGMENTS];
    uint32_t lengths[IO_SEGMENTS];

    for (int peer = 0; peer < io.nodes; peer++) {
        if (peer == io.local) {
            continue;
        }
        unsigned count = 0, bytes = 0, remaining = accessed;
        for (unsigned i = 0; i < num; i++) {
            unsigned length = write ? MIN(remaining, sg[i].iov_len) :
                                      sg[i].iov_len;
            if (write) {
                remaining -= length;
            }
            pthread_mutex_lock(&io.maps_lock);
            IOMapping *m = g_hash_table_lookup(io.maps, sg[i].iov_base);
            pthread_mutex_unlock(&io.maps_lock);
            if (!m || m->peer != peer || !length) {
                continue;
            }
            assert(m->write == write && length <= m->length);
            if (count == IO_SEGMENTS || length > IO_BYTES - bytes) {
                io_callv(peer, write, maps, lengths, count);
                count = bytes = 0;
            }
            maps[count] = m;
            lengths[count++] = length;
            bytes += length;
        }
        if (count) {
            io_callv(peer, write, maps, lengths, count);
        }
    }
}

void gvm_private_io_prepare(const VirtQueueElement *elem)
{
    io_element_transfer(elem, 0, false);
}

void gvm_private_io_complete(const VirtQueueElement *elem, unsigned accessed)
{
    io_element_transfer(elem, accessed, true);
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
        " remote_maps=%" PRIu64 " local_maps=%" PRIu64
        " mode=%u send_zcopy_bytes=%" PRIu64,
        qatomic_read(&io.calls), qatomic_read(&io.read_bytes),
        qatomic_read(&io.write_bytes), qatomic_read(&io.remote_maps),
        qatomic_read(&io.local_maps), io.mode, qatomic_read(&io.send_zcopy_bytes));
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
    const char *mode = getenv("GVM_PRIVATE_IO_ZCOPY");
    if (!mode || !strcmp(mode, "off")) {
        io.mode = 0;
    } else if (!strcmp(mode, "send")) {
        io.mode = 1;
    } else {
        io_fatal("unknown GVM_PRIVATE_IO_ZCOPY mode");
    }
    if (io.mode) {
        io.pool = qemu_memalign(IO_PAGE, IO_POOL_LIMIT);
        memset(io.pool, 0, IO_POOL_LIMIT);
        io.pool_used = bitmap_new(IO_POOL_PAGES);
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
    if (io.mode) {
        unsigned pages = DIV_ROUND_UP(length, IO_PAGE);
        unsigned first = bitmap_find_next_zero_area(io.pool_used, IO_POOL_PAGES,
                                                     0, pages, 0);
        if (first >= IO_POOL_PAGES || pages > IO_POOL_PAGES - first) {
            pthread_mutex_unlock(&io.maps_lock);
            g_free(m);
            *buffer = NULL;
            return true;
        }
        bitmap_set(io.pool_used, first, pages);
        m->buffer = io.pool + first * IO_PAGE;
    } else {
        m->buffer = qemu_memalign(IO_PAGE, length);
    }
    qatomic_add(&io.allocated, length);
    pthread_mutex_unlock(&io.maps_lock);
    if (write) {
        /* Do not expose uninitialized host data on a short backend read. */
        memset(m->buffer, 0, length);
    }
    /* Output descriptors are fetched together after the whole chain is parsed. */
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
    if (write && accessed && !m->published) {
        io_fatal("private buffer released before publication");
    }
    pthread_mutex_lock(&io.maps_lock);
    qatomic_sub(&io.allocated, m->length);
    if (io.mode) {
        bitmap_clear(io.pool_used, ((uint8_t *)m->buffer - io.pool) / IO_PAGE,
                     DIV_ROUND_UP(m->length, IO_PAGE));
    }
    pthread_mutex_unlock(&io.maps_lock);
    if (!io.mode) {
        qemu_vfree(m->buffer);
    }
    g_free(m);
    return true;
}
