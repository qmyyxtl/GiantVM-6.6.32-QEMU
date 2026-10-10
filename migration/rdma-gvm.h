#ifndef RDMA_GVM_H
#define RDMA_GVM_H

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stddef.h>

#define RDMA_BUFFER_COUNT 1024
#define RDMA_BUFFER_SIZE 4096
#define RDMA_CQ_SIZE 2048

// 消息头
typedef struct {
    uint16_t msg_type;
    uint16_t flags;
    uint32_t data_len;
    uint64_t sequence;
} rdma_msg_header_t;

#define MAX_MSG_DATA_SIZE (RDMA_BUFFER_SIZE - sizeof(rdma_msg_header_t))

// 初始化路由器
int rdma_router_init(int local_id, int total_nodes, char **hosts, int base_port);

// 发送请求并等待响应
int rdma_router_send_request(int target_id, uint16_t msg_type, 
                              const void *req_data, size_t req_len,
                              void *resp_data, size_t *resp_len);

/* Bounded, serialized RPC. Timeout means outcome unknown; never retry I/O. */
int rdma_router_call(int target_id, uint16_t msg_type,
                      const void *req_data, size_t req_len,
                      void *resp_data, size_t resp_capacity, size_t *resp_len,
                      unsigned timeout_ms);

/* One-way control messages bypass the ordinary response-CQ lock. */
int rdma_router_notify(int target_id, uint16_t type, const void *data,
                       size_t len);

// 发送响应
int rdma_router_send_response(int peer_id, uint16_t msg_type, uint64_t sequence,
                               const void *data, size_t len);

// 消息处理回调类型
typedef int (*msg_handler_t)(int peer_id, rdma_msg_header_t *hdr, void *data);

// 设置消息处理回调
void rdma_router_set_handler(msg_handler_t handler);
/* Return 1 for a consumed control message. Must not acquire the BQL or RPC. */
void rdma_router_set_control_handler(msg_handler_t handler);

// 清理
void rdma_router_cleanup(void);

#endif