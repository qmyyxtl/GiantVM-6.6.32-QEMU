#include "rdma-gvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>
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
    struct ibv_qp *qp;
    
    // Send buffers
    void *send_bufs[RDMA_BUFFER_COUNT];
    struct ibv_mr *send_mrs[RDMA_BUFFER_COUNT];
    int send_free_mask;  // bit mask: 1=free, 0=busy
    
    // Recv buffers
    void *recv_bufs[RDMA_BUFFER_COUNT];
    struct ibv_mr *recv_mrs[RDMA_BUFFER_COUNT];
    
    pthread_mutex_t send_lock;
    pthread_mutex_t recv_lock;
    
    uint64_t next_seq;  // 下一个发送的序列号

    pending_response_t *response_queue;
    pthread_mutex_t response_lock;
} rdma_conn_t;

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
    bool running;
} g_ctx;

// 消息处理回调
typedef int (*msg_handler_t)(int peer_id, rdma_msg_header_t *hdr, void *data);
static msg_handler_t g_msg_handler = NULL;

// 前向声明
static int create_connection(const char *host, int port, struct rdma_cm_id *server_cm_id, rdma_conn_t **out_conn);
static int post_all_recvs(rdma_conn_t *conn);
static int rdma_send_msg(rdma_conn_t *conn, rdma_msg_header_t *hdr, const void *data);
static int rdma_wait_completion(rdma_conn_t *conn, struct ibv_wc *wc);
static void* recv_thread_func(void *arg);

// ============ 初始化 ============
int rdma_router_init(int local_id, int total_nodes, char **hosts, int base_port)
{
    debugf("[RDMA Router] Initializing local_id=%d, total=%d\n", local_id, total_nodes);
    
    g_ctx.local_id = local_id;
    g_ctx.total_nodes = total_nodes;
    g_ctx.running = true;
    
    g_ctx.request_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.response_for_req_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.recv_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.reply_for_recv_conns = calloc(total_nodes, sizeof(rdma_conn_t*));
    g_ctx.recv_threads = calloc(total_nodes, sizeof(pthread_t));
    
    // ========== 阶段1：创建所有 listener ==========
    struct rdma_cm_id **listeners = calloc(total_nodes * 2, sizeof(struct rdma_cm_id*));
    
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        // Listener 1: 接收来自 i 的请求
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
        rdma_listen(listen_id1, 1);
        listeners[i*2] = listen_id1;
        
        debugf("[RDMA Router] Listening on %s:%d for requests from peer %d\n", 
               hosts[local_id], port1, i);
        
        // Listener 2: 接收来自 i 对我请求的响应
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
        rdma_listen(listen_id2, 1);
        listeners[i*2+1] = listen_id2;
        
        debugf("[RDMA Router] Listening on %s:%d for responses from peer %d\n",
               hosts[local_id], port2, i);
    }
    
    debugf("[RDMA Router] Waiting for all listeners to be ready...\n");
    sleep(3);
    
    // ========== 阶段2：建立连接（避免死锁：ID小的先连） ==========
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        if (local_id < i) {
            // 我的 ID 小，我主动连接
            
            // 连接1：发送请求到 i
            int port1 = base_port + local_id * total_nodes * 2 + i * 2;
            rdma_conn_t *conn1;
            debugf("[RDMA] Connecting to %s:%d (request channel)\n", hosts[i], port1);
            if (create_connection(hosts[i], port1, NULL, &conn1) < 0) {
                fprintf(stderr, "[RDMA Router] Failed to connect request channel to %d\n", i);
                return -1;
            }
            g_ctx.request_conns[i] = conn1;
            debugf("[RDMA Router] Connected request channel to peer %d\n", i);
            
            // 连接2：发送对 i 请求的回复
            int port2 = base_port + local_id * total_nodes * 2 + i * 2 + 1;
            rdma_conn_t *conn2;
            debugf("[RDMA] Connecting to %s:%d (reply channel)\n", hosts[i], port2);
            if (create_connection(hosts[i], port2, NULL, &conn2) < 0) {
                fprintf(stderr, "[RDMA Router] Failed to connect reply channel to %d\n", i);
                return -1;
            }
            g_ctx.reply_for_recv_conns[i] = conn2;
            debugf("[RDMA Router] Connected reply channel to peer %d\n", i);
        }
    }
    
    // ========== 阶段3：接受连接 ==========
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        if (local_id > i) {
            // 我的 ID 大，我接受连接
            
            // 接受1：接收来自 i 的请求
            debugf("[RDMA Router] Waiting for request connection from peer %d...\n", i);
            struct rdma_cm_event *event1;
            rdma_get_cm_event(listeners[i*2]->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *conn1;
            if (create_connection(NULL, 0, event1->id, &conn1) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr1;
            memset(&qp_attr1, 0, sizeof(qp_attr1));
            qp_attr1.send_cq = conn1->cq;
            qp_attr1.recv_cq = conn1->cq;
            qp_attr1.qp_type = IBV_QPT_RC;
            qp_attr1.cap.max_send_wr = RDMA_CQ_SIZE;
            qp_attr1.cap.max_recv_wr = RDMA_CQ_SIZE;
            qp_attr1.cap.max_send_sge = 1;
            qp_attr1.cap.max_recv_sge = 1;
            
            rdma_create_qp(conn1->cm_id, conn1->pd, &qp_attr1);
            conn1->qp = conn1->cm_id->qp;
            post_all_recvs(conn1);
            
            struct rdma_conn_param cm_params1;
            memset(&cm_params1, 0, sizeof(cm_params1));
            rdma_accept(conn1->cm_id, &cm_params1);
            rdma_ack_cm_event(event1);
            
            rdma_get_cm_event(conn1->cm_id->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event1);
            
            g_ctx.recv_conns[i] = conn1;
            rdma_destroy_id(listeners[i*2]);
            debugf("[RDMA Router] Accepted request connection from peer %d\n", i);
            
            // 接受2：接收来自 i 对我请求的响应
            debugf("[RDMA Router] Waiting for response connection from peer %d...\n", i);
            struct rdma_cm_event *event2;
            rdma_get_cm_event(listeners[i*2+1]->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *conn2;
            if (create_connection(NULL, 0, event2->id, &conn2) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr2;
            memset(&qp_attr2, 0, sizeof(qp_attr2));
            qp_attr2.send_cq = conn2->cq;
            qp_attr2.recv_cq = conn2->cq;
            qp_attr2.qp_type = IBV_QPT_RC;
            qp_attr2.cap.max_send_wr = RDMA_CQ_SIZE;
            qp_attr2.cap.max_recv_wr = RDMA_CQ_SIZE;
            qp_attr2.cap.max_send_sge = 1;
            qp_attr2.cap.max_recv_sge = 1;
            
            rdma_create_qp(conn2->cm_id, conn2->pd, &qp_attr2);
            conn2->qp = conn2->cm_id->qp;
            post_all_recvs(conn2);
            
            struct rdma_conn_param cm_params2;
            memset(&cm_params2, 0, sizeof(cm_params2));
            rdma_accept(conn2->cm_id, &cm_params2);
            rdma_ack_cm_event(event2);
            
            rdma_get_cm_event(conn2->cm_id->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event2);
            
            g_ctx.response_for_req_conns[i] = conn2;
            rdma_destroy_id(listeners[i*2+1]);
            debugf("[RDMA Router] Accepted response connection from peer %d\n", i);
        }
    }
    
    // ========== 阶段4：补充剩余连接 ==========
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        if (local_id < i) {
            // 我之前是主动方，现在被动接受
            
            debugf("[RDMA Router] Waiting for request connection from peer %d...\n", i);
            struct rdma_cm_event *event1;
            rdma_get_cm_event(listeners[i*2]->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *conn1;
            if (create_connection(NULL, 0, event1->id, &conn1) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr1;
            memset(&qp_attr1, 0, sizeof(qp_attr1));
            qp_attr1.send_cq = conn1->cq;
            qp_attr1.recv_cq = conn1->cq;
            qp_attr1.qp_type = IBV_QPT_RC;
            qp_attr1.cap.max_send_wr = RDMA_CQ_SIZE;
            qp_attr1.cap.max_recv_wr = RDMA_CQ_SIZE;
            qp_attr1.cap.max_send_sge = 1;
            qp_attr1.cap.max_recv_sge = 1;
            
            rdma_create_qp(conn1->cm_id, conn1->pd, &qp_attr1);
            conn1->qp = conn1->cm_id->qp;
            post_all_recvs(conn1);
            
            struct rdma_conn_param cm_params1;
            memset(&cm_params1, 0, sizeof(cm_params1));
            rdma_accept(conn1->cm_id, &cm_params1);
            rdma_ack_cm_event(event1);
            
            rdma_get_cm_event(conn1->cm_id->channel, &event1);
            if (event1->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event1);
            
            g_ctx.recv_conns[i] = conn1;
            rdma_destroy_id(listeners[i*2]);
            debugf("[RDMA Router] Accepted request connection from peer %d\n", i);
            
            // 接受响应连接
            debugf("[RDMA Router] Waiting for response connection from peer %d...\n", i);
            struct rdma_cm_event *event2;
            rdma_get_cm_event(listeners[i*2+1]->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
                fprintf(stderr, "[RDMA Router] Unexpected event\n");
                return -1;
            }
            
            rdma_conn_t *conn2;
            if (create_connection(NULL, 0, event2->id, &conn2) < 0) {
                return -1;
            }
            
            struct ibv_qp_init_attr qp_attr2;
            memset(&qp_attr2, 0, sizeof(qp_attr2));
            qp_attr2.send_cq = conn2->cq;
            qp_attr2.recv_cq = conn2->cq;
            qp_attr2.qp_type = IBV_QPT_RC;
            qp_attr2.cap.max_send_wr = RDMA_CQ_SIZE;
            qp_attr2.cap.max_recv_wr = RDMA_CQ_SIZE;
            qp_attr2.cap.max_send_sge = 1;
            qp_attr2.cap.max_recv_sge = 1;
            
            rdma_create_qp(conn2->cm_id, conn2->pd, &qp_attr2);
            conn2->qp = conn2->cm_id->qp;
            post_all_recvs(conn2);
            
            struct rdma_conn_param cm_params2;
            memset(&cm_params2, 0, sizeof(cm_params2));
            rdma_accept(conn2->cm_id, &cm_params2);
            rdma_ack_cm_event(event2);
            
            rdma_get_cm_event(conn2->cm_id->channel, &event2);
            if (event2->event != RDMA_CM_EVENT_ESTABLISHED) {
                fprintf(stderr, "[RDMA Router] Connection not established\n");
                return -1;
            }
            rdma_ack_cm_event(event2);
            
            g_ctx.response_for_req_conns[i] = conn2;
            rdma_destroy_id(listeners[i*2+1]);
            debugf("[RDMA Router] Accepted response connection from peer %d\n", i);
            
        } else {
            // 我之前是被动方，现在主动连接
            
            int port1 = base_port + local_id * total_nodes * 2 + i * 2;
            rdma_conn_t *conn1;
            debugf("[RDMA] Connecting to %s:%d (request channel)\n", hosts[i], port1);
            if (create_connection(hosts[i], port1, NULL, &conn1) < 0) {
                fprintf(stderr, "[RDMA Router] Failed to connect request channel to %d\n", i);
                return -1;
            }
            g_ctx.request_conns[i] = conn1;
            debugf("[RDMA Router] Connected request channel to peer %d\n", i);
            
            int port2 = base_port + local_id * total_nodes * 2 + i * 2 + 1;
            rdma_conn_t *conn2;
            debugf("[RDMA] Connecting to %s:%d (reply channel)\n", hosts[i], port2);
            if (create_connection(hosts[i], port2, NULL, &conn2) < 0) {
                fprintf(stderr, "[RDMA Router] Failed to connect reply channel to %d\n", i);
                return -1;
            }
            g_ctx.reply_for_recv_conns[i] = conn2;
            debugf("[RDMA Router] Connected reply channel to peer %d\n", i);
        }
    }
    
    free(listeners);
    
    // 启动接收线程
    for (int i = 0; i < total_nodes; i++) {
        if (i == local_id) continue;
        
        int *peer_id = malloc(sizeof(int));
        *peer_id = i;
        pthread_create(&g_ctx.recv_threads[i], NULL, recv_thread_func, peer_id);
    }
    
    debugf("[RDMA Router] Initialization complete\n");
    return 0;
}

// ============ 创建连接 ============

// 修改函数签名，服务器端传入 cm_id
static int create_connection(const char *host, int port, struct rdma_cm_id *server_cm_id, rdma_conn_t **out_conn)
{
    rdma_conn_t *conn = calloc(1, sizeof(rdma_conn_t));
    pthread_mutex_init(&conn->send_lock, NULL);
    pthread_mutex_init(&conn->recv_lock, NULL);
    pthread_mutex_init(&conn->response_lock, NULL);
    conn->send_free_mask = (1 << RDMA_BUFFER_COUNT) - 1;
    conn->next_seq = 1;
    conn->response_queue = NULL;
    
    if (server_cm_id) {
        // ========== 服务器端：使用传入的 cm_id ==========
        debugf("[RDMA] Creating server connection\n");
        conn->cm_id = server_cm_id;
        
        // 从 cm_id 的 verbs 创建资源
        conn->pd = ibv_alloc_pd(conn->cm_id->verbs);
        conn->cq = ibv_create_cq(conn->cm_id->verbs, RDMA_CQ_SIZE, NULL, NULL, 0);
        
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

static int rdma_send_msg(rdma_conn_t *conn, rdma_msg_header_t *hdr, const void *data)
{
    pthread_mutex_lock(&conn->send_lock);
    
    // Find free buffer
    int buf_idx = -1;
    for (int i = 0; i < RDMA_BUFFER_COUNT; i++) {
        if (conn->send_free_mask & (1 << i)) {
            buf_idx = i;
            conn->send_free_mask &= ~(1 << i);
            break;
        }
    }
    
    if (buf_idx < 0) {
        pthread_mutex_unlock(&conn->send_lock);
        fprintf(stderr, "[RDMA] No free send buffer\n");
        return -1;
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
        conn->send_free_mask |= (1 << buf_idx);
        pthread_mutex_unlock(&conn->send_lock);
        fprintf(stderr, "[RDMA] post_send failed\n");
        return -1;
    }
    
    // Wait for completion
    struct ibv_wc wc;
    if (rdma_wait_completion(conn, &wc) < 0 || wc.wr_id != buf_idx) {
        pthread_mutex_unlock(&conn->send_lock);
        return -1;
    }
    
    conn->send_free_mask |= (1 << buf_idx);
    pthread_mutex_unlock(&conn->send_lock);
    
    return sge.length;
}

static int rdma_wait_completion(rdma_conn_t *conn, struct ibv_wc *wc)
{
    while (1) {
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
}

// ============ 接收线程 ============

static void* recv_thread_func(void *arg)
{
    int peer_id = *(int*)arg;
    free(arg);
    
    rdma_conn_t *conn = g_ctx.recv_conns[peer_id];
    debugf("[RDMA Router] Recv thread started for peer %d\n", peer_id);
    
    while (g_ctx.running) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(conn->cq, 1, &wc);
        
        if (n < 0) {
            fprintf(stderr, "[RDMA] poll_cq error\n");
            break;
        }
        
        if (n == 0) {
            usleep(10);
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
            if (g_msg_handler) {
                g_msg_handler(peer_id, &hdr_copy, data_copy);
            }
            
            // 释放临时缓冲区
            if (data_copy) {
                free(data_copy);
            }
        }
    }
    
    return NULL;
}


// ============ API 实现 ============

int rdma_router_send_request(int target_id, uint16_t msg_type,
                              const void *req_data, size_t req_len,
                              void *resp_data, size_t *resp_len)
{
    rdma_conn_t *req_conn = g_ctx.request_conns[target_id];
    rdma_conn_t *resp_conn = g_ctx.response_for_req_conns[target_id];
    
    if (!req_conn) return -1;
    
    rdma_msg_header_t hdr;
    hdr.msg_type = msg_type;
    hdr.flags = 0;
    hdr.data_len = req_len;
    hdr.sequence = __sync_fetch_and_add(&req_conn->next_seq, 1);
    
    debugf("[RDMA Router] Sending request to %d: type=%d, len=%zu, seq=%lu\n",
           target_id, msg_type, req_len, hdr.sequence);
    
    // 发送请求
    if (rdma_send_msg(req_conn, &hdr, req_data) < 0) {
        return -1;
    }
    
    debugf("[RDMA Router] Sent request to %d: type=%d, len=%zu, seq=%lu\n",
           target_id, msg_type, req_len, hdr.sequence);
    
    // 🔑 关键修复：如果不需要响应（resp_data == NULL），直接返回
    if (resp_data == NULL) {
        debugf("[RDMA Router] No response needed for seq=%lu\n", hdr.sequence);
        return 0;
    }
    
    // 需要响应的情况，等待响应
    if (!resp_conn) {
        fprintf(stderr, "[RDMA Router] No response connection to %d\n", target_id);
        return -1;
    }
    
    debugf("[RDMA Router] Waiting for response from %d: seq=%lu\n", target_id, hdr.sequence);
    
    while (1) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(resp_conn->cq, 1, &wc);
        
        if (n == 1 && wc.opcode == IBV_WC_RECV) {
            int buf_idx = wc.wr_id;
            void *buf = resp_conn->recv_bufs[buf_idx];
            
            rdma_msg_header_t *resp_hdr = (rdma_msg_header_t*)buf;
            
            if (resp_hdr->sequence == hdr.sequence) {
                // 匹配！
                if (resp_hdr->data_len > 0) {
                    memcpy(resp_data, buf + sizeof(*resp_hdr), resp_hdr->data_len);
                }
                if (resp_len) *resp_len = resp_hdr->data_len;
                
                debugf("[RDMA Router] Found response from %d: seq=%lu\n", target_id, hdr.sequence);
                
                // Re-post
                struct ibv_sge sge;
                sge.addr = (uint64_t)buf;
                sge.length = RDMA_BUFFER_SIZE;
                sge.lkey = resp_conn->recv_mrs[buf_idx]->lkey;
                
                struct ibv_recv_wr wr, *bad_wr;
                memset(&wr, 0, sizeof(wr));
                wr.wr_id = buf_idx;
                wr.sg_list = &sge;
                wr.num_sge = 1;
                ibv_post_recv(resp_conn->qp, &wr, &bad_wr);
                
                return 0;
            }
            
            // 不匹配，re-post
            struct ibv_sge sge;
            sge.addr = (uint64_t)buf;
            sge.length = RDMA_BUFFER_SIZE;
            sge.lkey = resp_conn->recv_mrs[buf_idx]->lkey;
            
            struct ibv_recv_wr wr, *bad_wr;
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = buf_idx;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            ibv_post_recv(resp_conn->qp, &wr, &bad_wr);
        }
        
        usleep(1);
    }
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
    g_ctx.running = false;
    
    for (int i = 0; i < g_ctx.total_nodes; i++) {
        if (i != g_ctx.local_id) {
            pthread_join(g_ctx.recv_threads[i], NULL);
        }
    }
    
    // TODO: cleanup connections, buffers, etc.
}

// 设置消息处理回调
void rdma_router_set_handler(msg_handler_t handler)
{
    g_msg_handler = handler;
}