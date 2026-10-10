#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "qemu/atomic.h"
#include <poll.h>
#include <sys/eventfd.h>
#include "rdma-gvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdbool.h> 

// #define RDMA_DEBUG

#ifdef RDMA_DEBUG
#define debugf(...) printf(__VA_ARGS__)
#else
#define debugf(...) do {} while(0)
#endif


// pending response 
typedef struct pending_response {
    uint64_t sequence;
    uint8_t data[RDMA_BUFFER_SIZE];
    size_t data_len;
    struct pending_response *next;
} pending_response_t;

// RDMA 连接
typedef struct {
    struct rdma_cm_id *cm_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *cq_channel;
    bool cq_wait_enabled, cq_armed;
    struct ibv_qp *qp;
    
    // Send buffers
    void *send_bufs[RDMA_BUFFER_COUNT];
    struct ibv_mr *send_mrs[RDMA_BUFFER_COUNT];
    int send_free_mask;  // legacy field; the serialized sender uses buffer 0
    bool failed;
    
    // Recv buffers
    void *recv_bufs[RDMA_BUFFER_COUNT];
    struct ibv_mr *recv_mrs[RDMA_BUFFER_COUNT];
    
    pthread_mutex_t send_lock;
    pthread_mutex_t recv_lock;
    
    uint64_t next_seq;  // 下一个发送的序列号

    pending_response_t *response_queue;
    pthread_mutex_t response_lock;
} rdma_conn_t;

typedef struct router_work {
    rdma_msg_header_t hdr;
    void *data;
    struct router_work *next;
} router_work_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    router_work_t *head, *tail;
    unsigned queued;
    pthread_t thread;
} router_worker_t;

// 全局上下文
static struct {
    int local_id;
    int total_nodes;
    
    // 🔑 四条连接
    rdma_conn_t **request_conns;           // 发送请求
    rdma_conn_t **response_for_req_conns;  // 接收对我请求的响应
    rdma_conn_t **recv_conns;              // 接收请求
    rdma_conn_t **reply_for_recv_conns;    // 发送对请求的回复
    
    pthread_t *recv_threads;
    router_worker_t *workers;
    bool running;
    bool recv_cq_events; /* Immutable: channels created at initialization. */
    bool recv_cq_event_wait; /* Runtime receive policy, atomic access. */
    int stop_fd;
} g_ctx = { .stop_fd = -1 };

// 消息处理回调
typedef int (*msg_handler_t)(int peer_id, rdma_msg_header_t *hdr, void *data);
static msg_handler_t g_msg_handler = NULL;
static msg_handler_t g_control_handler;

// 前向声明
static int create_connection(const char *host, int port, struct rdma_cm_id *server_cm_id, rdma_conn_t **out_conn, bool event_receive);
static int post_all_recvs(rdma_conn_t *conn);
static int rdma_send_msg(rdma_conn_t *conn, rdma_msg_header_t *hdr, const void *data);
static int rdma_wait_completion(rdma_conn_t *conn, struct ibv_wc *wc);
static void* recv_thread_func(void *arg);
static void *request_worker_func(void *arg);

/*
 * Called by the main-loop control plane after router initialization. Connection
 * storage stays valid until cleanup; only the receiver changes its fallback
 * flag, which must be read atomically here. Never re-enable a failed channel.
 */
bool rdma_router_get_recv_cq_events(void)
{
    if (!qatomic_read(&g_ctx.running) ||
        !qatomic_read(&g_ctx.recv_cq_event_wait) || !g_ctx.recv_conns) {
        return false;
    }
    for (int i = 0; i < g_ctx.total_nodes; i++) {
        rdma_conn_t *conn = g_ctx.recv_conns[i];

        if (i != g_ctx.local_id &&
            (!conn || !qatomic_read(&conn->cq_wait_enabled))) {
            return false;
        }
    }
    return true;
}

int rdma_router_set_recv_cq_events(bool enabled)
{
    if (!qatomic_read(&g_ctx.running) || !g_ctx.recv_conns) {
        return -ESHUTDOWN;
    }
    if (enabled) {
        if (!g_ctx.recv_cq_events) {
            return -ENOTSUP;
        }
        for (int i = 0; i < g_ctx.total_nodes; i++) {
            rdma_conn_t *conn = g_ctx.recv_conns[i];

            if (i != g_ctx.local_id &&
                (!conn || !conn->cq_channel ||
                 !qatomic_read(&conn->cq_wait_enabled))) {
                return -ENOTSUP;
            }
        }
    }
    /*
     * A blocked receiver observes the new policy within its 100 ms poll bound.
     * Do not consume or write the shared stop fd for a configuration change.
     * Pending CQ events/armed state remain owned by the receiver and are
     * acknowledged on resuming event mode or during shutdown.
     */
    qatomic_set(&g_ctx.recv_cq_event_wait, enabled);
    return 0;
}

// ============ 初始化 ============
int rdma_router_init(int local_id, int total_nodes, char **hosts, int base_port)
{
    debugf("[RDMA Router] Initializing local_id=%d, total=%d\n", local_id, total_nodes);
    
    g_ctx.local_id = local_id;
    g_ctx.total_nodes = total_nodes;
    qatomic_set(&g_ctx.running, true);
    g_ctx.recv_cq_events =
        g_strcmp0(getenv("LOOM_RDMA_RECV_CQ_EVENTS"), "1") == 0;
    qatomic_set(&g_ctx.recv_cq_event_wait, g_ctx.recv_cq_events);
    if (g_ctx.recv_cq_events) {
        g_ctx.stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (g_ctx.stop_fd < 0) {
            return -1;
        }
    }
    
    g_ctx.request_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.response_for_req_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.recv_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.reply_for_recv_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.recv_threads = calloc(total_nodes, sizeof(pthread_t));
    g_ctx.workers = calloc(total_nodes, sizeof(*g_ctx.workers));
    
    // ========== 创建所有 listener ==========
    struct rdma_cm_id ***listeners = calloc(total_nodes, sizeof(struct rdma_cm_id**));
    for (int i = 0; i < total_nodes; i++) {
        listeners[i] = calloc(2, sizeof(struct rdma_cm_id*));
    }
    
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        // Listener 1: 接收请求
        int port1 = base_port + i * total_nodes * 2 + local_id * 2;
        struct rdma_cm_id *listen_id1;
        struct rdma_event_channel *cm_channel1 = rdma_create_event_channel();
        rdma_create_id(cm_channel1, &listen_id1, NULL, RDMA_PS_TCP);
        
        struct sockaddr_in addr1;
        memset(&addr1, 0, sizeof(addr1));
        addr1.sin_family = AF_INET;
        addr1.sin_addr.s_addr = inet_addr(hosts[local_id]);
        addr1.sin_port = htons(port1);
        
        rdma_bind_addr(listen_id1, (struct sockaddr*)&addr1);
        rdma_listen(listen_id1, 10);
        listeners[i][0] = listen_id1;
        
        debugf("[RDMA Router] Listening on %s:%d for requests from peer %d\n", 
               hosts[local_id], port1, i);
        
        // Listener 2: 接收响应
        int port2 = base_port + i * total_nodes * 2 + local_id * 2 + 1;
        struct rdma_cm_id *listen_id2;
        struct rdma_event_channel *cm_channel2 = rdma_create_event_channel();
        rdma_create_id(cm_channel2, &listen_id2, NULL, RDMA_PS_TCP);
        
        struct sockaddr_in addr2;
        memset(&addr2, 0, sizeof(addr2));
        addr2.sin_family = AF_INET;
        addr2.sin_addr.s_addr = inet_addr(hosts[local_id]);
        addr2.sin_port = htons(port2);
        
        rdma_bind_addr(listen_id2, (struct sockaddr*)&addr2);
        rdma_listen(listen_id2, 10);
        listeners[i][1] = listen_id2;
        
        debugf("[RDMA Router] Listening on %s:%d for responses from peer %d\n",
               hosts[local_id], port2, i);
    }
    
    debugf("[RDMA Router] Waiting for all listeners to be ready...\n");
    sleep(5);
    
    // ========== 🔑 关键改动：逐对建立连接 ==========
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        if (local_id < i) {
            // ===== 我的 ID 小，我先主动连接，再被动接受 =====
            
            debugf("[RDMA Router] Establishing connections with peer %d (I connect first)...\n", i);
            
            // 步骤1：主动连接 2 条
            int port1 = base_port + local_id * total_nodes * 2 + i * 2;
            rdma_conn_t *conn1;
            debugf("[RDMA] Connecting to %s:%d (request channel)\n", hosts[i], port1);
            int retry = 0;
            while (retry < 10) {
                if (create_connection(hosts[i], port1, NULL, &conn1, false) == 0) {
                    break;
                }
                retry++;
                sleep(1);
            }
            if (retry >= 10) {
                fprintf(stderr, "[RDMA Router] Failed to connect request channel to %d\n", i);
                return -1;
            }
            g_ctx.request_conns[i] = conn1;
            debugf("[RDMA Router] Connected request channel to peer %d\n", i);
            
            int port2 = base_port + local_id * total_nodes * 2 + i * 2 + 1;
            rdma_conn_t *conn2;
            debugf("[RDMA] Connecting to %s:%d (reply channel)\n", hosts[i], port2);
            retry = 0;
            while (retry < 10) {
                if (create_connection(hosts[i], port2, NULL, &conn2, false) == 0) {
                    break;
                }
                retry++;
                sleep(1);
            }
            if (retry >= 10) {
                fprintf(stderr, "[RDMA Router] Failed to connect reply channel to %d\n", i);
                return -1;
            }
            g_ctx.reply_for_recv_conns[i] = conn2;
            debugf("[RDMA Router] Connected reply channel to peer %d\n", i);
            
            // 步骤2：被动接受 2 条（对方会连接我）
            debugf("[RDMA Router] Waiting for request connection from peer %d...\n", i);
            struct rdma_cm_event *event1;
            rdma_get_cm_event(listeners[i][0]->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *recv_conn;
            if (create_connection(NULL, 0, event1->id, &recv_conn, true) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr1;
            memset(&qp_attr1, 0, sizeof(qp_attr1));
            qp_attr1.send_cq = recv_conn->cq;
            qp_attr1.recv_cq = recv_conn->cq;
            qp_attr1.qp_type = IBV_QPT_RC;
            qp_attr1.cap.max_send_wr = RDMA_BUFFER_COUNT;
            qp_attr1.cap.max_recv_wr = RDMA_BUFFER_COUNT;
            qp_attr1.cap.max_send_sge = 1;
            qp_attr1.cap.max_recv_sge = 1;
            
            rdma_create_qp(recv_conn->cm_id, recv_conn->pd, &qp_attr1);
            recv_conn->qp = recv_conn->cm_id->qp;
            post_all_recvs(recv_conn);
            
            struct rdma_conn_param cm_params1;
            memset(&cm_params1, 0, sizeof(cm_params1));
            cm_params1.retry_count = 7;
            cm_params1.rnr_retry_count = 7;
            rdma_accept(recv_conn->cm_id, &cm_params1);
            rdma_ack_cm_event(event1);
            
            rdma_get_cm_event(recv_conn->cm_id->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event1);
            
            g_ctx.recv_conns[i] = recv_conn;
            rdma_destroy_id(listeners[i][0]);
            debugf("[RDMA Router] Accepted request connection from peer %d\n", i);
            
            // 接受响应连接
            debugf("[RDMA Router] Waiting for response connection from peer %d...\n", i);
            struct rdma_cm_event *event2;
            rdma_get_cm_event(listeners[i][1]->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *resp_conn;
            if (create_connection(NULL, 0, event2->id, &resp_conn, false) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr2;
            memset(&qp_attr2, 0, sizeof(qp_attr2));
            qp_attr2.send_cq = resp_conn->cq;
            qp_attr2.recv_cq = resp_conn->cq;
            qp_attr2.qp_type = IBV_QPT_RC;
            qp_attr2.cap.max_send_wr = RDMA_BUFFER_COUNT;
            qp_attr2.cap.max_recv_wr = RDMA_BUFFER_COUNT;
            qp_attr2.cap.max_send_sge = 1;
            qp_attr2.cap.max_recv_sge = 1;
            
            rdma_create_qp(resp_conn->cm_id, resp_conn->pd, &qp_attr2);
            resp_conn->qp = resp_conn->cm_id->qp;
            post_all_recvs(resp_conn);
            
            struct rdma_conn_param cm_params2;
            memset(&cm_params2, 0, sizeof(cm_params2));
            cm_params2.retry_count = 7;
            cm_params2.rnr_retry_count = 7;
            rdma_accept(resp_conn->cm_id, &cm_params2);
            rdma_ack_cm_event(event2);
            
            rdma_get_cm_event(resp_conn->cm_id->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event2);
            
            g_ctx.response_for_req_conns[i] = resp_conn;
            rdma_destroy_id(listeners[i][1]);
            debugf("[RDMA Router] Accepted response connection from peer %d\n", i);
            
            debugf("[RDMA Router] All 4 connections established with peer %d\n", i);
            
        } else {
            // ===== 我的 ID 大，我先被动接受，再主动连接 =====
            
            debugf("[RDMA Router] Establishing connections with peer %d (I accept first)...\n", i);
            
            // 步骤1：被动接受 2 条（对方会先连接我）
            debugf("[RDMA Router] Waiting for request connection from peer %d...\n", i);
            struct rdma_cm_event *event1;
            rdma_get_cm_event(listeners[i][0]->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *recv_conn;
            if (create_connection(NULL, 0, event1->id, &recv_conn, true) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr1;
            memset(&qp_attr1, 0, sizeof(qp_attr1));
            qp_attr1.send_cq = recv_conn->cq;
            qp_attr1.recv_cq = recv_conn->cq;
            qp_attr1.qp_type = IBV_QPT_RC;
            qp_attr1.cap.max_send_wr = RDMA_BUFFER_COUNT;
            qp_attr1.cap.max_recv_wr = RDMA_BUFFER_COUNT;
            qp_attr1.cap.max_send_sge = 1;
            qp_attr1.cap.max_recv_sge = 1;
            
            rdma_create_qp(recv_conn->cm_id, recv_conn->pd, &qp_attr1);
            recv_conn->qp = recv_conn->cm_id->qp;
            post_all_recvs(recv_conn);
            
            struct rdma_conn_param cm_params1;
            memset(&cm_params1, 0, sizeof(cm_params1));
            cm_params1.retry_count = 7;
            cm_params1.rnr_retry_count = 7;
            rdma_accept(recv_conn->cm_id, &cm_params1);
            rdma_ack_cm_event(event1);
            
            rdma_get_cm_event(recv_conn->cm_id->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event1);
            
            g_ctx.recv_conns[i] = recv_conn;
            rdma_destroy_id(listeners[i][0]);
            debugf("[RDMA Router] Accepted request connection from peer %d\n", i);
            
            // 接受响应连接
            debugf("[RDMA Router] Waiting for response connection from peer %d...\n", i);
            struct rdma_cm_event *event2;
            rdma_get_cm_event(listeners[i][1]->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *resp_conn;
            if (create_connection(NULL, 0, event2->id, &resp_conn, false) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr2;
            memset(&qp_attr2, 0, sizeof(qp_attr2));
            qp_attr2.send_cq = resp_conn->cq;
            qp_attr2.recv_cq = resp_conn->cq;
            qp_attr2.qp_type = IBV_QPT_RC;
            qp_attr2.cap.max_send_wr = RDMA_BUFFER_COUNT;
            qp_attr2.cap.max_recv_wr = RDMA_BUFFER_COUNT;
            qp_attr2.cap.max_send_sge = 1;
            qp_attr2.cap.max_recv_sge = 1;
            
            rdma_create_qp(resp_conn->cm_id, resp_conn->pd, &qp_attr2);
            resp_conn->qp = resp_conn->cm_id->qp;
            post_all_recvs(resp_conn);
            
            struct rdma_conn_param cm_params2;
            memset(&cm_params2, 0, sizeof(cm_params2));
            cm_params2.retry_count = 7;
            cm_params2.rnr_retry_count = 7;
            rdma_accept(resp_conn->cm_id, &cm_params2);
            rdma_ack_cm_event(event2);
            
            rdma_get_cm_event(resp_conn->cm_id->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event2);
            
            g_ctx.response_for_req_conns[i] = resp_conn;
            rdma_destroy_id(listeners[i][1]);
            debugf("[RDMA Router] Accepted response connection from peer %d\n", i);
            
            // 步骤2：主动连接 2 条
            int port1 = base_port + local_id * total_nodes * 2 + i * 2;
            rdma_conn_t *conn1;
            debugf("[RDMA] Connecting to %s:%d (request channel)\n", hosts[i], port1);
            int retry = 0;
            while (retry < 10) {
                if (create_connection(hosts[i], port1, NULL, &conn1, false) == 0) {
                    break;
                }
                retry++;
                sleep(1);
            }
            if (retry >= 10) {
                fprintf(stderr, "[RDMA Router] Failed to connect request channel to %d\n", i);
                return -1;
            }
            g_ctx.request_conns[i] = conn1;
            debugf("[RDMA Router] Connected request channel to peer %d\n", i);
            
            int port2 = base_port + local_id * total_nodes * 2 + i * 2 + 1;
            rdma_conn_t *conn2;
            debugf("[RDMA] Connecting to %s:%d (reply channel)\n", hosts[i], port2);
            retry = 0;
            while (retry < 10) {
                if (create_connection(hosts[i], port2, NULL, &conn2, false) == 0) {
                    break;
                }
                retry++;
                sleep(1);
            }
            if (retry >= 10) {
                fprintf(stderr, "[RDMA Router] Failed to connect reply channel to %d\n", i);
                return -1;
            }
            g_ctx.reply_for_recv_conns[i] = conn2;
            debugf("[RDMA Router] Connected reply channel to peer %d\n", i);
            
            debugf("[RDMA Router] All 4 connections established with peer %d\n", i);
        }
    }
    
    // 清理 listeners
    for (int i = 0; i < total_nodes; i++) {
        free(listeners[i]);
    }
    free(listeners);
    
    // 启动接收线程
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        router_worker_t *worker = &g_ctx.workers[i];
        pthread_mutex_init(&worker->lock, NULL);
        pthread_cond_init(&worker->ready, NULL);
        int *work_peer = malloc(sizeof(int));
        *work_peer = i;
        if (pthread_create(&worker->thread, NULL, request_worker_func,
                           work_peer)) {
            return -1;
        }
        int *peer_id = malloc(sizeof(int));
        *peer_id = i;
        if (pthread_create(&g_ctx.recv_threads[i], NULL, recv_thread_func,
                           peer_id)) {
            return -1;
        }
    }
    
    debugf("[RDMA Router] Initialization complete\n");
    return 0;
}

// ============ 创建连接 ============

// 修改函数签名，服务器端传入 cm_id
static int create_connection(const char *host, int port, struct rdma_cm_id *server_cm_id, rdma_conn_t **out_conn, bool event_receive)
{
    rdma_conn_t *conn = calloc(1, sizeof(rdma_conn_t));
    pthread_mutex_init(&conn->send_lock, NULL);
    pthread_mutex_init(&conn->recv_lock, NULL);
    pthread_mutex_init(&conn->response_lock, NULL);
    conn->send_free_mask = 1;
    conn->next_seq = 1;
    conn->response_queue = NULL;
    
    if (server_cm_id) {
        // ========== 服务器端：使用传入的 cm_id ==========
        debugf("[RDMA] Creating server connection\n");
        conn->cm_id = server_cm_id;
        
        // 从 cm_id 的 verbs 创建资源
        conn->pd = ibv_alloc_pd(conn->cm_id->verbs);
        if (event_receive && g_ctx.recv_cq_events) {
            conn->cq_channel = ibv_create_comp_channel(conn->cm_id->verbs);
            if (conn->cq_channel) {
                int flags = fcntl(conn->cq_channel->fd, F_GETFL);
                if (flags < 0 ||
                    fcntl(conn->cq_channel->fd, F_SETFL,
                          flags | O_NONBLOCK) < 0) {
                    ibv_destroy_comp_channel(conn->cq_channel);
                    conn->cq_channel = NULL;
                }
            }
        }
        conn->cq = ibv_create_cq(conn->cm_id->verbs, RDMA_CQ_SIZE,
                                 conn->cq_channel ? conn : NULL,
                                 conn->cq_channel, 0);
        if (!conn->cq && conn->cq_channel) {
            ibv_destroy_comp_channel(conn->cq_channel);
            conn->cq_channel = NULL;
            conn->cq = ibv_create_cq(conn->cm_id->verbs, RDMA_CQ_SIZE,
                                     NULL, NULL, 0);
        }
        qatomic_set(&conn->cq_wait_enabled, conn->cq_channel != NULL);
        if (event_receive) {
            fprintf(stderr, "[RDMA] receive CQ mode: %s\n",
                    qatomic_read(&conn->cq_wait_enabled) ? "events" : "poll");
        }
        
    } else {
        // ========== 客户端：创建新的 cm_id 并连接 ==========
        debugf("[RDMA] Creating client connection to %s:%d\n", host, port);
        
        struct rdma_event_channel *cm_channel = rdma_create_event_channel();
        struct rdma_cm_id *cm_id;
        rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
        conn->cm_id = cm_id;
        
        // Resolve address
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons(port);
        
        debugf("[RDMA] Resolving address %s:%d\n", host, port);
        rdma_resolve_addr(cm_id, NULL, (struct sockaddr*)&addr, 2000);
        
        struct rdma_cm_event *event;
        rdma_get_cm_event(cm_channel, &event);
        if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
            fprintf(stderr, "[RDMA] Addr resolve failed: %d\n", event->event);
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
        
        debugf("[RDMA] Resolving route\n");
        rdma_resolve_route(cm_id, 2000);
        rdma_get_cm_event(cm_channel, &event);
        if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
            fprintf(stderr, "[RDMA] Route resolve failed: %d\n", event->event);
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
        
        // Create PD, CQ
        debugf("[RDMA] Creating resources\n");
        conn->pd = ibv_alloc_pd(cm_id->verbs);
        conn->cq = ibv_create_cq(cm_id->verbs, RDMA_CQ_SIZE, NULL, NULL, 0);
    }
    
    // ========== 公共部分：分配 buffers ==========
    debugf("[RDMA] Allocating buffers\n");
    for (int i = 0; i < RDMA_BUFFER_COUNT; i++) {
        conn->send_bufs[i] = malloc(RDMA_BUFFER_SIZE);
        conn->send_mrs[i] = ibv_reg_mr(conn->pd, conn->send_bufs[i], RDMA_BUFFER_SIZE,
                                        IBV_ACCESS_LOCAL_WRITE);
        
        conn->recv_bufs[i] = malloc(RDMA_BUFFER_SIZE);
        conn->recv_mrs[i] = ibv_reg_mr(conn->pd, conn->recv_bufs[i], RDMA_BUFFER_SIZE,
                                        IBV_ACCESS_LOCAL_WRITE);
    }
    debugf("[RDMA] Buffers allocated\n");
    
    if (!server_cm_id) {
        // ========== 客户端：创建 QP 并连接 ==========
        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof(qp_attr));
        qp_attr.send_cq = conn->cq;
        qp_attr.recv_cq = conn->cq;
        qp_attr.qp_type = IBV_QPT_RC;
        qp_attr.cap.max_send_wr = RDMA_CQ_SIZE;
        qp_attr.cap.max_recv_wr = RDMA_CQ_SIZE;
        qp_attr.cap.max_send_sge = 1;
        qp_attr.cap.max_recv_sge = 1;
        
        rdma_create_qp(conn->cm_id, conn->pd, &qp_attr);
        conn->qp = conn->cm_id->qp;
        
        // Post recvs
        post_all_recvs(conn);
        
        // Connect
        debugf("[RDMA] Connecting to %s:%d\n", host, port);
        struct rdma_conn_param cm_params;
        memset(&cm_params, 0, sizeof(cm_params));
        rdma_connect(conn->cm_id, &cm_params);
        
        struct rdma_cm_event *event;
        rdma_get_cm_event(conn->cm_id->channel, &event);
        if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
            fprintf(stderr, "[RDMA] Connection not established: %d\n", event->event);
            rdma_ack_cm_event(event);
            return -1;
        }
        rdma_ack_cm_event(event);
        debugf("[RDMA] Connected successfully\n");
    }
    // 服务器端的 QP 创建和 accept 在外面做
    
    *out_conn = conn;
    return 0;
}


// ============ 发送/接收 ============

static int post_all_recvs(rdma_conn_t *conn)
{
    for (int i = 0; i < RDMA_BUFFER_COUNT; i++) {
        struct ibv_sge sge;
        sge.addr = (uint64_t)conn->recv_bufs[i];
        sge.length = RDMA_BUFFER_SIZE;
        sge.lkey = conn->recv_mrs[i]->lkey;
        
        struct ibv_recv_wr wr, *bad_wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        
        if (ibv_post_recv(conn->qp, &wr, &bad_wr)) {
            fprintf(stderr, "[RDMA] post_recv failed\n");
            return -1;
        }
    }
    return 0;
}

static uint64_t rdma_monotonic_ms(void);

static int rdma_send_msg(rdma_conn_t *conn, rdma_msg_header_t *hdr, const void *data)
{
    pthread_mutex_lock(&conn->send_lock);
    
    /* send_lock and synchronous completion make one send buffer enough.
     * A 32-bit bitmask cannot describe RDMA_BUFFER_COUNT (1024) entries. */
    int buf_idx = 0;
    if (hdr->data_len > MAX_MSG_DATA_SIZE || (hdr->data_len && !data)) {
        pthread_mutex_unlock(&conn->send_lock);
        return -EMSGSIZE;
    }

    if (conn->failed) {
        pthread_mutex_unlock(&conn->send_lock);
        return -ENOTCONN;
    }
    // Copy message
    void *buf = conn->send_bufs[buf_idx];
    memcpy(buf, hdr, sizeof(*hdr));
    if (data && hdr->data_len > 0) {
        memcpy(buf + sizeof(*hdr), data, hdr->data_len);
    }
    
    // Post send
    struct ibv_sge sge;
    sge.addr = (uint64_t)buf;
    sge.length = sizeof(*hdr) + hdr->data_len;
    sge.lkey = conn->send_mrs[buf_idx]->lkey;
    
    struct ibv_send_wr wr, *bad_wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = buf_idx;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    
    if (ibv_post_send(conn->qp, &wr, &bad_wr)) {
        conn->failed = true;
        conn->send_free_mask |= (1 << buf_idx);
        pthread_mutex_unlock(&conn->send_lock);
        fprintf(stderr, "[RDMA] post_send failed\n");
        return -1;
    }
    
    // Wait for completion
    struct ibv_wc wc;
    if (rdma_wait_completion(conn, &wc) < 0 || wc.wr_id != buf_idx) {
        conn->failed = true;
        pthread_mutex_unlock(&conn->send_lock);
        return -1;
    }
    
    conn->send_free_mask |= (1 << buf_idx);
    pthread_mutex_unlock(&conn->send_lock);
    
    return sge.length;
}

static int rdma_wait_completion(rdma_conn_t *conn, struct ibv_wc *wc)
{
    uint64_t deadline = rdma_monotonic_ms() + 5000;
    while (rdma_monotonic_ms() < deadline) {
        int n = ibv_poll_cq(conn->cq, 1, wc);
        if (n < 0) {
            fprintf(stderr, "[RDMA] poll_cq failed\n");
            return -1;
        }
        if (n == 1) {
            if (wc->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[RDMA] WC error: %s\n", ibv_wc_status_str(wc->status));
                return -1;
            }
            return 0;
        }
        usleep(1);
    }
    return -ETIMEDOUT;
}

// ============ 接收线程 ============

/*
 * Keep normal requests FIFO per peer, but never block control reception on
 * a model callback, BQL acquisition, or a nested RPC in that callback.
 */
static void *request_worker_func(void *arg)
{
    int peer = *(int *)arg;
    router_worker_t *worker = &g_ctx.workers[peer];

    free(arg);
    pthread_setname_np(pthread_self(), "gvm-rdma-req");
    rcu_register_thread();
    for (;;) {
        router_work_t *work;
        pthread_mutex_lock(&worker->lock);
        while (qatomic_read(&g_ctx.running) && !worker->head) {
            pthread_cond_wait(&worker->ready, &worker->lock);
        }
        if (!qatomic_read(&g_ctx.running)) {
            pthread_mutex_unlock(&worker->lock);
            break;
        }
        work = worker->head;
        worker->head = work->next;
        worker->queued--;
        if (!worker->head) {
            worker->tail = NULL;
        }
        pthread_mutex_unlock(&worker->lock);
        if (g_msg_handler) {
            g_msg_handler(peer, &work->hdr, work->data);
        }
        free(work->data);
        free(work);
    }
    rcu_unregister_thread();
    return NULL;
}


/*
 * One consumer owns each request receive CQ. Arm before draining, then wait
 * for its event channel; an extra event without a CQE is valid. The stop fd
 * remains readable for every receiver until all threads have joined.
 */
static int rdma_recv_completion(rdma_conn_t *conn, struct ibv_wc *wc)
{
    while (qatomic_read(&g_ctx.running)) {
        int n;
        bool wait_events = qatomic_read(&g_ctx.recv_cq_event_wait) &&
                           qatomic_read(&conn->cq_wait_enabled);

        if (wait_events && !conn->cq_armed) {
            if (ibv_req_notify_cq(conn->cq, 0)) {
                fprintf(stderr, "[RDMA] CQ notify failed; using polling\n");
                qatomic_set(&conn->cq_wait_enabled, false);
            } else {
                conn->cq_armed = true;
            }
        }
        n = ibv_poll_cq(conn->cq, 1, wc);
        if (n) {
            return n;
        }
        if (!wait_events || !qatomic_read(&conn->cq_wait_enabled)) {
            usleep(10);
            return 0;
        }

        struct pollfd fds[2] = {
            { .fd = conn->cq_channel->fd, .events = POLLIN },
            { .fd = g_ctx.stop_fd, .events = POLLIN },
        };
        int ret = poll(fds, 2, 100);

        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "[RDMA] CQ event wait failed; using polling\n");
            qatomic_set(&conn->cq_wait_enabled, false);
            continue;
        }
        if (!qatomic_read(&g_ctx.running) || (fds[1].revents & POLLIN)) {
            return 0;
        }
        if (fds[0].revents & POLLIN) {
            struct ibv_cq *cq;
            void *context;

            if (ibv_get_cq_event(conn->cq_channel, &cq, &context)) {
                if (errno != EAGAIN && errno != EINTR) {
                    fprintf(stderr, "[RDMA] CQ event read failed; using polling\n");
                    qatomic_set(&conn->cq_wait_enabled, false);
                }
                continue;
            }
            ibv_ack_cq_events(cq, 1);
            if (cq != conn->cq) {
                fprintf(stderr, "[RDMA] unexpected CQ event; using polling\n");
                qatomic_set(&conn->cq_wait_enabled, false);
                continue;
            }
            conn->cq_armed = false;
        }
    }
    return 0;
}

static void* recv_thread_func(void *arg)
{
    int peer_id = *(int*)arg;
    free(arg);
    pthread_setname_np(pthread_self(), "gvm-rdma-rx");
    
    rdma_conn_t *conn = g_ctx.recv_conns[peer_id];
    debugf("[RDMA Router] Recv thread started for peer %d\n", peer_id);
    
    while (qatomic_read(&g_ctx.running)) {
        struct ibv_wc wc;
        int n = rdma_recv_completion(conn, &wc);
        
        if (n < 0) {
            fprintf(stderr, "[RDMA] poll_cq error\n");
            break;
        }
        
        if (n == 0) {
            continue;
        }
        
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[RDMA] WC error: %s\n", ibv_wc_status_str(wc.status));
            continue;
        }
        
        if (wc.opcode == IBV_WC_RECV) {
            int buf_idx = wc.wr_id;
            void *buf = conn->recv_bufs[buf_idx];
            
            rdma_msg_header_t *hdr = (rdma_msg_header_t*)buf;
            if (wc.byte_len < sizeof(*hdr) || hdr->data_len > MAX_MSG_DATA_SIZE ||
                hdr->data_len > wc.byte_len - sizeof(*hdr)) {
                fprintf(stderr, "[RDMA] Malformed request\n");
                qatomic_set(&g_ctx.running, false);
                break;
            }
            void *data = buf + sizeof(*hdr);
            
            debugf("[RDMA Router] Received request from peer %d: type=%d, len=%d, seq=%lu\n",
                   peer_id, hdr->msg_type, hdr->data_len, hdr->sequence);
            
            // 🔑 关键修改：先复制消息到临时缓冲区
            rdma_msg_header_t hdr_copy = *hdr;
            void *data_copy = NULL;
            if (hdr->data_len > 0) {
                data_copy = malloc(hdr->data_len);
                memcpy(data_copy, data, hdr->data_len);
            }
            
            // 🔑 立即 re-post recv（在处理消息之前！）
            struct ibv_sge sge;
            sge.addr = (uint64_t)buf;
            sge.length = RDMA_BUFFER_SIZE;
            sge.lkey = conn->recv_mrs[buf_idx]->lkey;
            
            struct ibv_recv_wr wr, *bad_wr;
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = buf_idx;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            
            if (ibv_post_recv(conn->qp, &wr, &bad_wr) != 0) {
                fprintf(stderr, "[RDMA] Failed to re-post recv\n");
            }
            
            // 🔑 现在可以慢慢处理消息了（使用副本）
            debugf("[RDMA Router] Handling message from peer %d\n", peer_id);
            if (g_control_handler &&
                g_control_handler(peer_id, &hdr_copy, data_copy)) {
                free(data_copy);
            } else {
                router_worker_t *worker = &g_ctx.workers[peer_id];
                router_work_t *work = calloc(1, sizeof(*work));
                if (!work) {
                    fprintf(stderr, "[RDMA] request queue allocation failed\n");
                    abort();
                }
                work->hdr = hdr_copy;
                work->data = data_copy;
                pthread_mutex_lock(&worker->lock);
                if (worker->queued == RDMA_BUFFER_COUNT) {
                    fprintf(stderr, "[RDMA] request queue exhausted\n");
                    abort();
                }
                worker->queued++;
                if (worker->tail) {
                    worker->tail->next = work;
                } else {
                    worker->head = work;
                }
                worker->tail = work;
                pthread_cond_signal(&worker->ready);
                pthread_mutex_unlock(&worker->lock);
            }
        }
    }
    
    return NULL;
}


// ============ API 实现 ============


static uint64_t rdma_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int rdma_repost_response(rdma_conn_t *conn, unsigned index)
{
    struct ibv_sge sge = {
        .addr = (uint64_t)conn->recv_bufs[index],
        .length = RDMA_BUFFER_SIZE,
        .lkey = conn->recv_mrs[index]->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id = index, .sg_list = &sge, .num_sge = 1,
    }, *bad;
    return ibv_post_recv(conn->qp, &wr, &bad) ? -EIO : 0;
}

int rdma_router_call(int target_id, uint16_t msg_type,
                      const void *req_data, size_t req_len,
                      void *resp_data, size_t resp_capacity, size_t *resp_len,
                      unsigned timeout_ms)
{
    rdma_conn_t *req, *resp;
    rdma_msg_header_t hdr;
    uint64_t deadline;
    int result = -EIO;

    if (target_id < 0 || target_id >= g_ctx.total_nodes ||
        req_len > MAX_MSG_DATA_SIZE || (req_len && !req_data)) {
        return -EINVAL;
    }
    req = g_ctx.request_conns[target_id];
    resp = g_ctx.response_for_req_conns[target_id];
    if (!req || (resp_data && !resp)) {
        return -ENOTCONN;
    }
    /* Every response CQ has one consumer, including legacy callers. */
    pthread_mutex_lock(&req->response_lock);
    hdr = (rdma_msg_header_t) {
        .msg_type = msg_type, .flags = resp_data ? 0 : 1,
        .data_len = req_len,
        .sequence = __sync_fetch_and_add(&req->next_seq, 1),
    };
    deadline = rdma_monotonic_ms() + timeout_ms;
    if (rdma_send_msg(req, &hdr, req_data) < 0) {
        goto out;
    }
    if (!resp_data) {
        result = 0;
        goto out;
    }
    while (rdma_monotonic_ms() < deadline) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(resp->cq, 1, &wc);
        if (n < 0) {
            goto out;
        }
        if (!n) {
            usleep(1);
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV ||
            wc.wr_id >= RDMA_BUFFER_COUNT || wc.byte_len < sizeof(hdr)) {
            result = -EPROTO;
            goto out;
        }
        rdma_msg_header_t *reply = resp->recv_bufs[wc.wr_id];
        if (reply->data_len > MAX_MSG_DATA_SIZE ||
            reply->data_len > wc.byte_len - sizeof(*reply)) {
            rdma_repost_response(resp, wc.wr_id);
            result = -EPROTO;
            goto out;
        }
        if (reply->sequence != hdr.sequence) {
            /* A response to a timed-out operation has no live waiter. */
            rdma_repost_response(resp, wc.wr_id);
            continue;
        }
        if (reply->msg_type != msg_type || reply->data_len > resp_capacity) {
            rdma_repost_response(resp, wc.wr_id);
            result = -EMSGSIZE;
            goto out;
        }
        memcpy(resp_data, (uint8_t *)reply + sizeof(*reply), reply->data_len);
        if (resp_len) {
            *resp_len = reply->data_len;
        }
        result = rdma_repost_response(resp, wc.wr_id);
        goto out;
    }
    result = -ETIMEDOUT;
out:
    pthread_mutex_unlock(&req->response_lock);
    return result;
}

int rdma_router_send_request(int target_id, uint16_t msg_type,
                              const void *req_data, size_t req_len,
                              void *resp_data, size_t *resp_len)
{
    return rdma_router_call(target_id, msg_type, req_data, req_len,
                            resp_data, MAX_MSG_DATA_SIZE, resp_len, 30000);
}

int rdma_router_notify(int target_id, uint16_t type, const void *data,
                       size_t len)
{
    rdma_conn_t *conn;
    rdma_msg_header_t hdr;

    if (target_id < 0 || target_id >= g_ctx.total_nodes ||
        len > MAX_MSG_DATA_SIZE || (len && !data)) {
        return -EINVAL;
    }
    conn = g_ctx.request_conns[target_id];
    if (!conn) {
        return -ENOTCONN;
    }
    hdr = (rdma_msg_header_t) {
        .msg_type = type, .flags = 1, .data_len = len,
        .sequence = __sync_fetch_and_add(&conn->next_seq, 1),
    };
    return rdma_send_msg(conn, &hdr, data) < 0 ? -EIO : 0;
}

int rdma_router_send_response(int peer_id, uint16_t msg_type, uint64_t sequence,
                               const void *data, size_t len)
{
    rdma_conn_t *conn = g_ctx.reply_for_recv_conns[peer_id];  // 专用回复通道
    if (!conn) return -1;
    
    rdma_msg_header_t hdr;
    hdr.msg_type = msg_type;
    hdr.flags = 1;
    hdr.data_len = len;
    hdr.sequence = sequence;
    
    return rdma_send_msg(conn, &hdr, data);
}

void rdma_router_cleanup(void)
{
    qatomic_set(&g_ctx.running, false);
    if (g_ctx.stop_fd >= 0) {
        uint64_t wake = 1;
        ssize_t ret = write(g_ctx.stop_fd, &wake, sizeof(wake));

        if (ret < 0 && errno != EAGAIN) {
            fprintf(stderr, "[RDMA] receiver stop wake failed: %s\n",
                    strerror(errno));
        }
    }
    
    for (int i = 0; i < g_ctx.total_nodes; i++) {
        if (i != g_ctx.local_id) {
            router_worker_t *worker = &g_ctx.workers[i];
            pthread_mutex_lock(&worker->lock);
            pthread_cond_broadcast(&worker->ready);
            pthread_mutex_unlock(&worker->lock);
            pthread_join(g_ctx.recv_threads[i], NULL);
            pthread_join(worker->thread, NULL);
            while (worker->head) {
                router_work_t *work = worker->head;
                worker->head = work->next;
                free(work->data);
                free(work);
            }
            pthread_cond_destroy(&worker->ready);
            pthread_mutex_destroy(&worker->lock);
        }
    }
    
    /*
     * Release only event-mode receive CQ resources after all consumers exit.
     * Stop the QP first so no further completion events can be generated.
     * Other legacy connection resources keep their existing lifetime.
     */
    for (int i = 0; i < g_ctx.total_nodes; i++) {
        rdma_conn_t *conn = g_ctx.recv_conns[i];

        if (!conn || !conn->cq_channel) {
            continue;
        }
        rdma_destroy_qp(conn->cm_id);
        conn->qp = NULL;
        struct ibv_cq *cq;
        void *context;
        for (;;) {
            if (ibv_get_cq_event(conn->cq_channel, &cq, &context) == 0) {
                ibv_ack_cq_events(cq, 1);
            } else if (errno != EINTR) {
                break;
            }
        }
        if (ibv_destroy_cq(conn->cq)) {
            fprintf(stderr, "[RDMA] receive CQ destroy failed\n");
            continue;
        }
        conn->cq = NULL;
        ibv_destroy_comp_channel(conn->cq_channel);
        conn->cq_channel = NULL;
    }
    if (g_ctx.stop_fd >= 0) {
        close(g_ctx.stop_fd);
        g_ctx.stop_fd = -1;
    }
    // TODO: cleanup connections, buffers, etc.
}

// 设置消息处理回调
void rdma_router_set_handler(msg_handler_t handler)
{
    g_msg_handler = handler;
}
void rdma_router_set_control_handler(msg_handler_t handler)
{
    g_control_handler = handler;
}
