#include "qemu/osdep.h"
#include <linux/kvm.h>
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/i386/apic.h"
#include "hw/intc/ioapic.h"
#include "hw/i386/kvm/clock.h"
#include "hw/core/cpu.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "interrupt-router.h"
#include "qom/object_interfaces.h"
#include "hw/boards.h"
#include "migration/rdma-gvm.h"
#include "hw/pci/loom.h"
#include "sysemu/kvm.h"
#include "qemu/main-loop.h"

// #define RDMA_DEBUG

#define		APIC_ICR_BUSY		0x01000

#ifdef RDMA_DEBUG
#define debugf(...) printf(__VA_ARGS__)
#else
#define debugf(...) do {} while(0)
#endif

#define ROUTER_BUFFER_SIZE 4096
#define CPU_INDEX_ANY -1

/* 消息类型定义 */
enum forward_type {
    PIO = 1,
    MMIO,
    LAPIC,
    SPECIAL_INT,
    SIPI,
    X2APIC,
    APIC_BASE,
    INIT_LEVEL_DEASSERT,
    FIXED_INT,
    IOAPIC_INI,
    IOAPIC,
    KVMCLOCK,
    INI_ROUTER,
    SHUTDOWN,
    RESET,
    EXIT,
    LOOM_PCI,
    LOOM_DIRECTORY,
    LOOM_INTX,
    LOOM_MSI,
    LOOM_DIRECTORY_UPDATE,
    LOOM_DIRECTORY_ACK,
};


typedef struct LoomDirectoryRequest {
    uint32_t offset, size;
} LoomDirectoryRequest;
typedef struct LoomDirectoryResponse {
    int32_t status;
    uint32_t size;
    uint8_t data[LOOM_DIRECTORY_CHUNK];
} LoomDirectoryResponse;

typedef struct LoomDirectoryUpdate {
    uint64_t serial;
    uint32_t device_id, size;
    uint8_t data[LOOM_DIRECTORY_UPDATE_SIZE];
} LoomDirectoryUpdate;

typedef struct LoomDirectoryAck {
    uint64_t serial;
    int32_t status;
    uint32_t reserved;
} LoomDirectoryAck;

static int loom_notify(unsigned peer, uint16_t type, const void *payload,
                        size_t size)
{
    uint8_t data[MAX_MSG_DATA_SIZE];
    int32_t cpu = CPU_INDEX_ANY;

    if (size > sizeof(data) - sizeof(cpu)) {
        return -EMSGSIZE;
    }
    memcpy(data, &cpu, sizeof(cpu));
    memcpy(data + sizeof(cpu), payload, size);
    return rdma_router_notify(peer, type, data, size + sizeof(cpu));
}

/* Runs directly in the CQ receive thread, never under the BQL. */
static int handle_directory_control(int peer, rdma_msg_header_t *hdr,
                                    void *data)
{
    uint8_t *payload = data;
    if (hdr->msg_type != LOOM_DIRECTORY_UPDATE &&
        hdr->msg_type != LOOM_DIRECTORY_ACK) {
        return 0;
    }
    if (!loom_pci_enabled() || hdr->flags != 1 ||
        hdr->data_len < sizeof(int32_t)) {
        error_report("Loom PCI: malformed directory control message");
        exit(EXIT_FAILURE);
    }
    payload += sizeof(int32_t);
    if (hdr->msg_type == LOOM_DIRECTORY_UPDATE) {
        LoomDirectoryUpdate update;
        LoomDirectoryAck ack = { 0 };
        if (hdr->data_len != sizeof(int32_t) + sizeof(update)) {
            error_report("Loom PCI: malformed directory update");
            exit(EXIT_FAILURE);
        }
        memcpy(&update, payload, sizeof(update));
        ack.serial = update.serial;
        ack.status = loom_pci_directory_update_dispatch(peer, update.device_id,
                                                       update.data,
                                                       update.size);
        if (loom_notify(peer, LOOM_DIRECTORY_ACK, &ack, sizeof(ack)) < 0) {
            error_report("Loom PCI: directory acknowledgement failed");
            exit(EXIT_FAILURE);
        }
    } else {
        LoomDirectoryAck ack;
        if (hdr->data_len != sizeof(int32_t) + sizeof(ack)) {
            error_report("Loom PCI: malformed directory acknowledgement");
            exit(EXIT_FAILURE);
        }
        memcpy(&ack, payload, sizeof(ack));
        loom_pci_directory_ack_dispatch(peer, ack.serial, ack.status);
    }
    return 1;
}

int loom_pci_directory_push(unsigned peer, uint64_t serial, unsigned device_id,
                            const void *data, unsigned size)
{
    LoomDirectoryUpdate update = {
        .serial = serial, .device_id = device_id, .size = size,
    };
    if (size != sizeof(update.data)) {
        return -EINVAL;
    }
    memcpy(update.data, data, size);
    return loom_notify(peer, LOOM_DIRECTORY_UPDATE, &update, sizeof(update));
}

QemuMutex ipi_mutex;
QemuMutex io_forwarding_mutex;

static uint32_t router_hosts_num = 0;
static char **router_hosts = NULL;

/* ============ 辅助函数 ============ */

int parse_cluster_iplist(const char *cluster_iplist)
{
    int ret = 0, ip_num = 0, i;

    if (cluster_iplist == NULL) {
        ret = -EINVAL;
        goto out;
    }

    for (i = 0; cluster_iplist[i] != '\0'; i++) {
        if (isspace(cluster_iplist[i]))
            ip_num++;
        else if (!(isdigit(cluster_iplist[i]) || cluster_iplist[i] == '.')) {
            ret = -EINVAL;
            goto out;
        }
    }
    router_hosts_num = ++ip_num;

    router_hosts = (char **)g_malloc0(sizeof(char *) * ip_num);

    char *temp_iplist = (char *)g_malloc(strlen(cluster_iplist) + 1);
    strcpy(temp_iplist, cluster_iplist);
    char *parse_ip = strtok(temp_iplist, " ");
    i = 0;
    while (parse_ip != NULL) {
        char *ip = (char *)g_malloc(20);
        strcpy(ip, parse_ip);
        router_hosts[i++] = ip;
        parse_ip = strtok(NULL, " ");
    }
    g_free(temp_iplist);

out:
    return ret;
}

char **get_cluster_iplist(uint32_t *len)
{
    *len = router_hosts_num;
    return router_hosts;
}

static void kvm_handle_remote_io(uint16_t port, MemTxAttrs attrs, void *data, 
                                  int direction, int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs,
                         ptr, size,
                         direction == KVM_EXIT_IO_OUT);
        ptr += size;
    }
}

/* ============ 消息处理回调 ============ */

static int handle_incoming_message(int peer_id, rdma_msg_header_t *hdr, void *data)
{
    uint8_t *ptr = data;
    int cpu_index;
    
    /* 解析 CPU index（所有消息都有） */
    if (hdr->data_len < sizeof(int32_t)) {
        error_report("Message too short");
        return -1;
    }
    
    cpu_index = *(int32_t*)ptr;
    ptr += sizeof(int32_t);
    
    if (cpu_index != CPU_INDEX_ANY) {
        current_cpu = qemu_get_cpu(cpu_index);
    }
    debugf("[RDMA Router] Handling message from peer %d: type=%d, len=%d, seq=%lu, cpu_index=%d\n",
           peer_id, hdr->msg_type, hdr->data_len, hdr->sequence, cpu_index);
    /* 处理消息 */
    switch (hdr->msg_type) {
    case LOOM_PCI: {
        LoomPCIRequest request;
        LoomPCIResponse response = { .status = -EINVAL };
        if (hdr->data_len == sizeof(int32_t) + sizeof(request)) {
            memcpy(&request, ptr, sizeof(request));
            loom_pci_dispatch(&request, &response);
        }
        rdma_router_send_response(peer_id, LOOM_PCI, hdr->sequence,
                                  &response, sizeof(response));
        break;
    }
    case LOOM_DIRECTORY: {
        LoomDirectoryRequest request;
        LoomDirectoryResponse response = { .status = -EINVAL };
        if (hdr->data_len == sizeof(int32_t) + sizeof(request)) {
            memcpy(&request, ptr, sizeof(request));
            loom_pci_directory_dispatch(request.offset, response.data,
                                         request.size, &response.status);
            if (!response.status) {
                response.size = request.size;
            }
        }
        rdma_router_send_response(peer_id, LOOM_DIRECTORY, hdr->sequence,
                                  &response, offsetof(LoomDirectoryResponse, data) +
                                  response.size);
        break;
    }
    case LOOM_INTX: {
        LoomPCIRequest request;
        if (hdr->data_len == sizeof(int32_t) + sizeof(request) &&
            MACHINE(qdev_get_machine())->local_cpu_start_index == 0) {
            memcpy(&request, ptr, sizeof(request));
            if (request.device_id / 64 == peer_id) {
                loom_pci_intx_dispatch(&request);
            }
        }
        break;
    }
    case LOOM_MSI: {
        MSIMessage message;
        int32_t status = -EINVAL;
        MachineState *ms = MACHINE(qdev_get_machine());
        if (hdr->data_len == sizeof(int32_t) + sizeof(message)) {
            memcpy(&message, ptr, sizeof(message));
            unsigned target = (message.address >> 12) & 0xff;
            if (!(message.address & 4) && !(message.data & 0x700) &&
                target >= ms->local_cpu_start_index &&
                target < ms->local_cpu_start_index + ms->local_cpus) {
                status = loom_pci_inject_msi(message);
            }
        }
        if (!hdr->flags) {
            rdma_router_send_response(peer_id, LOOM_MSI, hdr->sequence,
                                      &status, sizeof(status));
        }
        break;
    }
    case PIO: {
        /* 解析 PIO 数据 */
        uint16_t port = *(uint16_t*)ptr;
        ptr += sizeof(uint16_t);
        MemTxAttrs attrs = *(MemTxAttrs*)ptr;
        ptr += sizeof(MemTxAttrs);
        int direction = *(int*)ptr;
        ptr += sizeof(int);
        int size = *(int*)ptr;
        ptr += sizeof(int);
        uint32_t count = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        void *pio_data = ptr;
        
        /* 执行 PIO */
        kvm_handle_remote_io(port, attrs, pio_data, direction, size, count);
        
        /* 如果是输入操作，需要回复 */
        if (direction == KVM_EXIT_IO_IN && hdr->flags == 0) {
            rdma_router_send_response(peer_id, PIO, hdr->sequence, 
                                      pio_data, count * size);
        }
        break;
    }
    
    case MMIO: {
        /* 解析 MMIO 数据 */
        hwaddr addr = *(hwaddr*)ptr;
        ptr += sizeof(hwaddr);
        MemTxAttrs attrs = *(MemTxAttrs*)ptr;
        ptr += sizeof(MemTxAttrs);
        int len = *(int*)ptr;
        ptr += sizeof(int);
        bool is_write = *(bool*)ptr;
        ptr += sizeof(bool);
        void *mmio_data = ptr;
        
        /* 执行 MMIO */
        if (!loom_pci_handle_mmio(addr, attrs, mmio_data, len, is_write)) {
            address_space_rw(&address_space_memory, addr, attrs,
                             mmio_data, len, is_write);
        }
        
        /* 如果是读操作，需要回复 */
        if (!is_write && hdr->flags == 0) {
            rdma_router_send_response(peer_id, MMIO, hdr->sequence, 
                                      mmio_data, len);
        }
        break;
    }
    
    case LAPIC: {
        hwaddr addr = *(hwaddr*)ptr;
        ptr += sizeof(hwaddr);
        uint32_t val = *(uint32_t*)ptr;
        
        apic_lapic_write(current_cpu, addr, val);
        break;
    }
    
    case SPECIAL_INT: {
        int mask = *(int*)ptr;
        cpu_interrupt(current_cpu, mask);
        break;
    }
    
    case SIPI: {
        uint32_t val = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        uint32_t val2 = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        uint32_t dest_id = *(uint32_t*)ptr;
        
        int ret = kvm_dipi_forwarding(cpu_index, val, val2,dest_id);
        if (ret < 0) {
            error_report("KVM DSM IPI fail");
        }
        printf("SIPI received: cpu_index=%d, val=0x%08x, val2=0x%08x, dest_id=%d\n",
               cpu_index, val, val2,dest_id);
        
         /* 回复 SIPI 响应 */
        // if (val == 0x300) {
        //         //     if (delivery_mode == 0x500 || delivery_mode == 0x600)
        //     {
        //          /* 回复 SIPI 响应 */
        //         uint32_t response = 99;
        //         rdma_router_send_response(peer_id, SIPI, hdr->sequence, 
        //                                 &response, sizeof(response));
        //     }
        // }
        // else if (val == 0x310)
        // {
        //     uint32_t response = 99;
        //     rdma_router_send_response(peer_id, SIPI, hdr->sequence, 
        //                             &response, sizeof(response));
        // }
        
        break;
    }
    
    case X2APIC: {
        __u64 x2data = *(__u64*)ptr;
        int ret = kvm_x2apic_handle(cpu_index, x2data);
        if (ret < 0) {
            error_report("KVM DSM x2apic fail");
        }
        break;
    }
    
    case APIC_BASE: {
        int host = *(int*)ptr;
        ptr += sizeof(int);
        uint32_t base_index = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        __u64 base_data = *(__u64*)ptr; 
        
        int ret = kvm_apic_base_handle(cpu_index, host, base_index, base_data);
        if (ret < 0) {
            error_report("KVM DSM APIC_BASE fail");
        }
        break;
    }
    
    case INIT_LEVEL_DEASSERT: {
        apic_init_level_deassert(current_cpu);
        break;
    }
    
    case FIXED_INT: {
        int vector_num = *(int*)ptr;
        ptr += sizeof(int);
        int trigger_mode = *(int*)ptr;
        
        apic_set_irq_detour(current_cpu, vector_num, trigger_mode);
        break;
    }
    
    case IOAPIC_INI: {
        uint32_t val = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        uint32_t val2 = *(uint32_t*)ptr;
        
        kvm_ioapic_irq_handle(val, val2);
        break;
    }
    
    case IOAPIC: {
        int isrv = *(int*)ptr;
        ioapic_eoi_broadcast(isrv);
        break;
    }
    
    case KVMCLOCK: {
        /* 获取 KVM 时钟并回复 */
        uint64_t kvmclock = kvmclock_getclock();
        rdma_router_send_response(peer_id, KVMCLOCK, hdr->sequence, 
                                  &kvmclock, sizeof(kvmclock));
        break;
    }
    
    case INI_ROUTER: {
        /* 解析路由表 */
        uint32_t nr = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        uint32_t flags = *(uint32_t*)ptr;
        ptr += sizeof(uint32_t);
        
        struct kvm_irq_routing *routing = malloc(sizeof(*routing) + 
                                                  nr * sizeof(struct kvm_irq_routing_entry));
        routing->nr = nr;
        routing->flags = flags;
        
        for (uint32_t i = 0; i < nr; i++) {
            struct kvm_irq_routing_entry *entry = &(routing->entries[i]);
            entry->gsi = *(uint32_t*)ptr;
            ptr += sizeof(uint32_t);
            entry->type = *(uint32_t*)ptr;
            ptr += sizeof(uint32_t);
            entry->flags = *(uint32_t*)ptr;
            ptr += sizeof(uint32_t);
            entry->pad = *(uint32_t*)ptr;
            ptr += sizeof(uint32_t);
            
            switch (entry->type) {
            case KVM_IRQ_ROUTING_IRQCHIP:
                entry->u.irqchip.irqchip = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                entry->u.irqchip.pin = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                break;
            case KVM_IRQ_ROUTING_MSI:
                entry->u.msi.address_lo = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                entry->u.msi.address_hi = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                entry->u.msi.data = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                entry->u.msi.pad = *(uint32_t*)ptr;
                ptr += sizeof(uint32_t);
                break;
            default:
                error_report("Unknown router type[%u]", entry->type);
                free(routing);
                return -1;
            }
        }
        
        kvm_ioapic_router_handle(routing);
        
        free(routing);
        break;
    }
    
    case SHUTDOWN:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        break;
        
    case RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
        
    case EXIT:
        exit(0);
        
    default:
        error_report("Unknown message type[%u]", hdr->msg_type);
        return -1;
    }
    
    return 0;
}

/* ============ 启动/关闭 ============ */

static bool router_get_recv_cq_events(Object *obj, Error **errp)
{
    return rdma_router_get_recv_cq_events();
}

static void router_set_recv_cq_events(Object *obj, bool enabled, Error **errp)
{
    int ret = rdma_router_set_recv_cq_events(enabled);

    if (ret < 0) {
        error_setg_errno(errp, -ret,
                         "RDMA receive CQ event mode unavailable; "
                         "healthy channels must be created at startup");
    }
}

static void router_get_rpc_spin_ns(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t budget = rdma_router_get_rpc_spin_ns();

    visit_type_uint64(v, name, &budget, errp);
}

static void router_set_rpc_spin_ns(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint64_t budget;

    if (!visit_type_uint64(v, name, &budget, errp)) {
        return;
    }
    if (rdma_router_set_rpc_spin_ns(budget) < 0) {
        error_setg(errp, "RDMA RPC spin budget must be in 0..20000 ns");
    }
}

void start_io_router(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    
    qemu_mutex_init(&io_forwarding_mutex);
    qemu_mutex_init(&ipi_mutex);
    
    if (ms->local_cpus == ms->smp.cpus) {
        printf("Single QEMU instance, router not needed\n");
        return;
    }
    
    if (router_hosts_num != ms->qemu_nums) {
        error_report("Invalid number of cluster iplist");
        exit(1);
    }
    
    printf("Starting RDMA router for QEMU %d (total: %d)\n",
           ms->local_cpu_start_index / ms->local_cpus, ms->qemu_nums);
    
    /* 设置消息处理回调 */
    rdma_router_set_handler(handle_incoming_message);
    rdma_router_set_control_handler(handle_directory_control);
    
    /* 初始化 RDMA 路由器 */
    int ret = rdma_router_init(ms->local_cpu_start_index / ms->local_cpus,
                               ms->qemu_nums,
                               router_hosts,
                               40000);
    
    if (ret < 0) {
        error_report("Failed to initialize RDMA router");
        exit(1);
    }
    
    object_property_add_bool(OBJECT(ms), "loom-rdma-recv-cq-events",
                             router_get_recv_cq_events,
                             router_set_recv_cq_events);
    object_property_add(OBJECT(ms), "loom-rdma-rpc-spin-ns", "uint64",
                        router_get_rpc_spin_ns, router_set_rpc_spin_ns,
                        NULL, NULL);
    printf("RDMA router started successfully\n");
}

void disconnect_io_router(void)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    
    if (ms->local_cpus == ms->smp.cpus) {
        return;
    }
    
    rdma_router_cleanup();
    
    for (int i = 0; i < router_hosts_num; i++) {
        g_free(router_hosts[i]);
    }
    g_free(router_hosts);
}

/* ============ 消息转发函数 ============ */

void pio_forwarding(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                    int size, uint32_t count, bool broadcast)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据：cpu_index + PIO 数据 */
    size_t data_size = sizeof(int32_t) + sizeof(uint16_t) + sizeof(MemTxAttrs) + 
                       sizeof(int) * 2 + sizeof(uint32_t) + (count * size);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = current_cpu ? current_cpu->cpu_index : CPU_INDEX_ANY;
    ptr += sizeof(int32_t);
    *(uint16_t*)ptr = port;
    ptr += sizeof(uint16_t);
    *(MemTxAttrs*)ptr = attrs;
    ptr += sizeof(MemTxAttrs);
    *(int*)ptr = direction;
    ptr += sizeof(int);
    *(int*)ptr = size;
    ptr += sizeof(int);
    *(uint32_t*)ptr = count;
    ptr += sizeof(uint32_t);
    memcpy(ptr, data, count * size);
    debugf("[RDMA Router] PIO forwarding: port=0x%04x, direction=%s, size=%d, count=%u, broadcast=%d\n",
           port, direction == KVM_EXIT_IO_IN ? "IN" : "OUT", size, count, broadcast);
    if (broadcast) {
        /* 广播到所有其他 QEMU 实例 */
        for (int i = 0; i < ms->qemu_nums; i++) {
            if (i == ms->local_cpu_start_index / ms->local_cpus) {
                continue;
            }
            
            if (direction == KVM_EXIT_IO_IN) {
                /* 需要响应 */
                size_t resp_len;
                rdma_router_send_request(i, PIO, msg_data, data_size, 
                                        data, &resp_len);
            } else {
                /* 不需要响应 */
                rdma_router_send_request(i, PIO, msg_data, data_size, 
                                        NULL, NULL);
            }
        }
    } else {
        /* 单播到 BSP (QEMU 0) */
        if (direction == KVM_EXIT_IO_IN) {
            size_t resp_len;
            rdma_router_send_request(0, PIO, msg_data, data_size, 
                                    data, &resp_len);
        } else {
            rdma_router_send_request(0, PIO, msg_data, data_size, 
                                    NULL, NULL);
        }
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void mmio_forwarding(hwaddr addr, MemTxAttrs attrs, uint8_t *data, int len, bool is_write)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] MMIO forwarding: addr=0x%llx, len=%d, is_write=%d\n",
           (unsigned long long)addr, len, is_write);
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(hwaddr) + sizeof(MemTxAttrs) + 
                       sizeof(int) + sizeof(bool) + len;
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = current_cpu ? current_cpu->cpu_index : CPU_INDEX_ANY;
    ptr += sizeof(int32_t);
    *(hwaddr*)ptr = addr;
    ptr += sizeof(hwaddr);
    *(MemTxAttrs*)ptr = attrs;
    ptr += sizeof(MemTxAttrs);
    *(int*)ptr = len;
    ptr += sizeof(int);
    *(bool*)ptr = is_write;
    ptr += sizeof(bool);
    memcpy(ptr, data, len);
    
    if (!is_write) {
        /* 读操作，需要响应 */
        size_t resp_len;
        rdma_router_send_request(0, MMIO, msg_data, data_size, 
                                data, &resp_len);
    } else {
        /* 写操作，不需要响应 */
        rdma_router_send_request(0, MMIO, msg_data, data_size, 
                                NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void lapic_forwarding(int cpu_index, hwaddr addr, uint32_t val)
{
    
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] LAPIC forwarding: cpu_index=%d, addr=0x%llx, val=0x%08x\n",
           cpu_index, (unsigned long long)addr, val);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(hwaddr) + sizeof(uint32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(hwaddr*)ptr = addr;
    ptr += sizeof(hwaddr);
    *(uint32_t*)ptr = val;
    
    /* 广播到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == ms->local_cpu_start_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, LAPIC, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void special_interrupt_forwarding(int cpu_index, int mask)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] Special interrupt forwarding: cpu_index=%d, mask=0x%08x\n",
           cpu_index, mask);
    MachineState *ms = MACHINE(qdev_get_machine());
    int target_qemu = cpu_index / ms->local_cpus;
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(int);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(int*)ptr = mask;
    
    rdma_router_send_request(target_qemu, SPECIAL_INT, msg_data, data_size, 
                            NULL, NULL);
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void startup_forwarding(int cpu_index, uint32_t val, uint32_t val2,uint32_t dest_id)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    // printf("[RDMA Router] Startup forwarding: cpu_index=%d, val=0x%08x, val2=0x%08x\n",
    //        cpu_index, val, val2);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(uint32_t) * 3;
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(uint32_t*)ptr = val;
    ptr += sizeof(uint32_t);
    *(uint32_t*)ptr = val2;
    ptr += sizeof(uint32_t);
    *(uint32_t*)ptr = dest_id;
    /* 发送到所有其他 QEMU 实例（除了目标 CPU 所在的） */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == cpu_index / ms->local_cpus) {
            // kvm_dipi_forwarding(cpu_index, val, val2,dest_id);
            continue;
        }
        else
        {   
            rdma_router_send_request(i, SIPI, msg_data, data_size, NULL, NULL);
        }
        
    
    }
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void x2apic_forwarding(int cpu_index, __u64 data)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] x2APIC forwarding: cpu_index=%d, data=0x%016llx\n",
           cpu_index, (unsigned long long)data);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(__u64);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(__u64*)ptr = data;  // 使用 __u64
    
    /* 发送到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == cpu_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, X2APIC, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void apic_base_forwarding(int cpu_index, bool host, uint32_t index, __u64 data)  // 不是 uint64_t
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] APIC_BASE forwarding: cpu_index=%d, host=%d, index=%u, data=0x%016llx\n",
           cpu_index, host, index, (unsigned long long)data);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(int) + sizeof(uint32_t) + sizeof(__u64);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(int*)ptr = (int)host;
    ptr += sizeof(int);
    *(uint32_t*)ptr = index;
    ptr += sizeof(uint32_t);
    *(__u64*)ptr = data;  // 使用 __u64
    
    /* 发送到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == cpu_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, APIC_BASE, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void init_level_deassert_forwarding(int cpu_index)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] INIT_LEVEL_DEASSERT forwarding: cpu_index=%d\n",
           cpu_index);
    MachineState *ms = MACHINE(qdev_get_machine());
    int target_qemu = cpu_index / ms->local_cpus;
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    *(int32_t*)msg_data = cpu_index;
    
    rdma_router_send_request(target_qemu, INIT_LEVEL_DEASSERT, msg_data, data_size, 
                            NULL, NULL);
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void irq_forwarding(int cpu_index, int vector_num, int trigger_mode)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] IRQ forwarding: cpu_index=%d, vector_num=%d, trigger_mode=%d\n",
           cpu_index, vector_num, trigger_mode);
    MachineState *ms = MACHINE(qdev_get_machine());
    int target_qemu = cpu_index / ms->local_cpus;
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(int) * 2;
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = cpu_index;
    ptr += sizeof(int32_t);
    *(int*)ptr = vector_num;
    ptr += sizeof(int);
    *(int*)ptr = trigger_mode;
    
    rdma_router_send_request(target_qemu, FIXED_INT, msg_data, data_size, 
                            NULL, NULL);
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void ioapic_irq_forwarding(int irq, int level)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] IOAPIC IRQ forwarding: irq=%d, level=%d\n",
           irq, level);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(int) * 2;
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = CPU_INDEX_ANY;
    ptr += sizeof(int32_t);
    *(int*)ptr = irq;
    ptr += sizeof(int);
    *(int*)ptr = level;
    
    /* 发送到所有其他 QEMU 实例（除了 QEMU 0） */
    for (int i = 1; i < ms->qemu_nums; i++) {
        rdma_router_send_request(i, IOAPIC_INI, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void ioapic_router_forwarding(struct kvm_irq_routing *routing)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] IOAPIC router forwarding: nr=%u, flags=0x%08x\n",
           routing->nr, routing->flags);
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 计算消息大小 */
    size_t data_size = sizeof(int32_t) + sizeof(uint32_t) * 2;
    for (uint32_t i = 0; i < routing->nr; i++) {
        data_size += sizeof(uint32_t) * 4;
        switch (routing->entries[i].type) {
        case KVM_IRQ_ROUTING_IRQCHIP:
            data_size += sizeof(uint32_t) * 2;
            break;
        case KVM_IRQ_ROUTING_MSI:
            data_size += sizeof(uint32_t) * 4;
            break;
        }
    }
    
    /* 准备消息数据 */
    uint8_t *msg_data = g_malloc(data_size);
    uint8_t *ptr = msg_data;
    
    *(int32_t*)ptr = CPU_INDEX_ANY;
    ptr += sizeof(int32_t);
    *(uint32_t*)ptr = routing->nr;
    ptr += sizeof(uint32_t);
    *(uint32_t*)ptr = routing->flags;
    ptr += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < routing->nr; i++) {
        struct kvm_irq_routing_entry *entry = &(routing->entries[i]);
        
        *(uint32_t*)ptr = entry->gsi;
        ptr += sizeof(uint32_t);
        *(uint32_t*)ptr = entry->type;
        ptr += sizeof(uint32_t);
        *(uint32_t*)ptr = entry->flags;
        ptr += sizeof(uint32_t);
        *(uint32_t*)ptr = entry->pad;
        ptr += sizeof(uint32_t);
        
        switch (entry->type) {
        case KVM_IRQ_ROUTING_IRQCHIP:
            *(uint32_t*)ptr = entry->u.irqchip.irqchip;
            ptr += sizeof(uint32_t);
            *(uint32_t*)ptr = entry->u.irqchip.pin;
            ptr += sizeof(uint32_t);
            break;
        case KVM_IRQ_ROUTING_MSI:
            *(uint32_t*)ptr = entry->u.msi.address_lo;
            ptr += sizeof(uint32_t);
            *(uint32_t*)ptr = entry->u.msi.address_hi;
            ptr += sizeof(uint32_t);
            *(uint32_t*)ptr = entry->u.msi.data;
            ptr += sizeof(uint32_t);
            *(uint32_t*)ptr = entry->u.msi.pad;
            ptr += sizeof(uint32_t);
            break;
        }
    }
    
    /* 发送到所有其他 QEMU 实例（除了 QEMU 0） */
    for (int i = 1; i < ms->qemu_nums; i++) {
        rdma_router_send_request(i, INI_ROUTER, msg_data, data_size, 
                                NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void eoi_forwarding(int isrv)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] EOI forwarding: isrv=%d\n",
           isrv);
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t) + sizeof(int);
    uint8_t *msg_data = g_malloc(data_size);
    
    uint8_t *ptr = msg_data;
    *(int32_t*)ptr = CPU_INDEX_ANY;
    ptr += sizeof(int32_t);
    *(int*)ptr = isrv;
    
    rdma_router_send_request(0, IOAPIC, msg_data, data_size, NULL, NULL);
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void kvmclock_fetching(uint64_t *kvmclock)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] KVM clock fetching\n");
    /* 准备消息数据（只有 cpu_index） */
    size_t data_size = sizeof(int32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    *(int32_t*)msg_data = CPU_INDEX_ANY;
    
    size_t resp_len;
    rdma_router_send_request(0, KVMCLOCK, msg_data, data_size, 
                            kvmclock, &resp_len);
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void shutdown_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] Shutdown forwarding\n");
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    *(int32_t*)msg_data = CPU_INDEX_ANY;
    
    /* 广播到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == ms->local_cpu_start_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, SHUTDOWN, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void reset_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] Reset forwarding\n");
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    *(int32_t*)msg_data = CPU_INDEX_ANY;
    
    /* 广播到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == ms->local_cpu_start_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, RESET, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}

void exit_forwarding(void)
{
    qemu_mutex_lock(&io_forwarding_mutex);
    debugf("[RDMA Router] Exit forwarding\n");
    MachineState *ms = MACHINE(qdev_get_machine());
    
    /* 准备消息数据 */
    size_t data_size = sizeof(int32_t);
    uint8_t *msg_data = g_malloc(data_size);
    
    *(int32_t*)msg_data = CPU_INDEX_ANY;
    
    /* 广播到所有其他 QEMU 实例 */
    for (int i = 0; i < ms->qemu_nums; i++) {
        if (i == ms->local_cpu_start_index / ms->local_cpus) {
            continue;
        }
        rdma_router_send_request(i, EXIT, msg_data, data_size, NULL, NULL);
    }
    
    g_free(msg_data);
    qemu_mutex_unlock(&io_forwarding_mutex);
}
/* Separate request and response QPs allow the peer handler to progress.
 * Drop the BQL before waiting for any owner to acquire its BQL.
 */
static int loom_rpc(int owner, uint16_t type, const void *payload, size_t size,
                    void *response, size_t capacity, size_t *length)
{
    uint8_t data[MAX_MSG_DATA_SIZE];
    int32_t cpu = CPU_INDEX_ANY;
    bool locked = bql_locked();
    int result;

    if (size > sizeof(data) - sizeof(cpu)) {
        return -EMSGSIZE;
    }
    memcpy(data, &cpu, sizeof(cpu));
    memcpy(data + sizeof(cpu), payload, size);
    if (locked) {
        bql_unlock();
    }
    result = rdma_router_call(owner, type, data, size + sizeof(cpu),
                              response, capacity, length, 5000);
    if (locked) {
        bql_lock();
    }
    return result;
}

int loom_pci_rpc(int owner, const LoomPCIRequest *req, LoomPCIResponse *resp)
{
    size_t length = 0;
    int result = loom_rpc(owner, LOOM_PCI, req, sizeof(*req), resp,
                          sizeof(*resp), &length);
    return result < 0 ? result : length == sizeof(*resp) ? 0 : -EPROTO;
}

int loom_pci_directory_rpc(int owner, uint32_t offset, void *buffer, uint32_t size)
{
    uint8_t *out = buffer;
    while (size) {
        LoomDirectoryRequest req = { offset, MIN(size, LOOM_DIRECTORY_CHUNK) };
        LoomDirectoryResponse resp;
        size_t length = 0;
        int result = loom_rpc(owner, LOOM_DIRECTORY, &req, sizeof(req),
                              &resp, sizeof(resp), &length);
        if (result < 0) {
            return result;
        }
        if (length < offsetof(LoomDirectoryResponse, data) || resp.status ||
            resp.size != req.size ||
            length != offsetof(LoomDirectoryResponse, data) + resp.size) {
            return -EPROTO;
        }
        memcpy(out, resp.data, resp.size);
        out += resp.size;
        offset += resp.size;
        size -= resp.size;
    }
    return 0;
}

static void loom_send_intx(void *opaque)
{
    LoomPCIRequest *req = opaque;
    loom_rpc(0, LOOM_INTX, req, sizeof(*req), NULL, 0, NULL);
    g_free(req);
}

void loom_pci_intx_notify(const LoomPCIRequest *req)
{
    /* Do not send while the owner's PCI/BQL callback is active. */
    aio_bh_schedule_oneshot(qemu_get_aio_context(), loom_send_intx,
                            g_memdup2(req, sizeof(*req)));
}

static void loom_send_msi(void *opaque)
{
    MSIMessage *msg = opaque;
    MachineState *ms = MACHINE(qdev_get_machine());
    int owner = ((msg->address >> 12) & 0xff) / ms->local_cpus;
    int32_t status;
    size_t length = 0;
    int result = loom_rpc(owner, LOOM_MSI, msg, sizeof(*msg), &status,
                          sizeof(status), &length);
    g_free(msg);
    if (result < 0 || length != sizeof(status) || status < 0) {
        error_report("Loom RDMA: remote MSI delivery failed");
        exit(EXIT_FAILURE);
    }
}

bool loom_pci_forward_msi(MSIMessage msg, int *status)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    unsigned cpu, owner;
    if (!loom_pci_enabled()) {
        return false;
    }
    /* Initial routing contract: physical, fixed, xAPIC destinations. */
    if ((msg.address & 4) || (msg.address >> 32) || (msg.data & 0x700)) {
        *status = -EOPNOTSUPP;
        return true;
    }
    cpu = (msg.address >> 12) & 0xff;
    if (cpu >= ms->smp.cpus) {
        *status = -EINVAL;
        return true;
    }
    owner = cpu / ms->local_cpus;
    if (owner == ms->local_cpu_start_index / ms->local_cpus) {
        return false;
    }
    /* Queue after the model callback; simultaneous cross-owner MMIO must
     * not recurse into a response QP already owned by another vCPU. */
    aio_bh_schedule_oneshot(qemu_get_aio_context(), loom_send_msi,
                            g_memdup2(&msg, sizeof(msg)));
    *status = 0;
    return true;
}
