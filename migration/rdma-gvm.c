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
                if (create_connection(hosts[i], port1, NULL, &conn1) == 0) {
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
                if (create_connection(hosts[i], port2, NULL, &conn2) == 0) {
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
            if (create_connection(NULL, 0, event1->id, &recv_conn) < 0) {
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
            if (create_connection(NULL, 0, event2->id, &resp_conn) < 0) {
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
            if (create_connection(NULL, 0, event1->id, &recv_conn) < 0) {
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
            if (create_connection(NULL, 0, event2->id, &resp_conn) < 0) {
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
                if (create_connection(hosts[i], port1, NULL, &conn1) == 0) {
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
                if (create_connection(hosts[i], port2, NULL, &conn2) == 0) {
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
    conn->send_free_mask = 1;
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
    
    /* send_lock and synchronous completion make one send buffer enough.
     * A 32-bit bitmask cannot describe RDMA_BUFFER_COUNT (1024) entries. */
    int buf_idx = 0;
    if (hdr->data_len > MAX_MSG_DATA_SIZE || (hdr->data_len && !data)) {
        pthread_mutex_unlock(&conn->send_lock);
        return -EMSGSIZE;
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
            if (wc.byte_len < sizeof(*hdr) || hdr->data_len > MAX_MSG_DATA_SIZE ||
                hdr->data_len > wc.byte_len - sizeof(*hdr)) {
                fprintf(stderr, "[RDMA] Malformed request\n");
                g_ctx.running = false;
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