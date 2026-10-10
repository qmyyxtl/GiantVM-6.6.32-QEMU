/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_PCI_LOOM_H
#define HW_PCI_LOOM_H

#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"

enum {
    LOOM_PCI_CONFIG_READ = 1,
    LOOM_PCI_CONFIG_WRITE,
    LOOM_PCI_BAR_READ,
    LOOM_PCI_BAR_WRITE,
    LOOM_PCI_INTX,
    LOOM_PCI_RESET,
    LOOM_IOMMU_READ,
    LOOM_IOMMU_WRITE,
};

/* Same-version peers; fixed-width requests contain no host pointers. */
typedef struct LoomPCIRequest {
    uint32_t operation;
    uint32_t device_id;
    uint32_t generation;
    uint32_t bar;
    uint64_t offset;
    uint64_t value;
    uint32_t size;
    uint32_t reserved;
} LoomPCIRequest;

typedef struct LoomPCIResponse {
    int32_t status;
    uint32_t generation;
    uint64_t value;
} LoomPCIResponse;

#define LOOM_IOMMU_MAX_NODES 4
#define LOOM_IOMMU_BASE UINT64_C(0xfed90000)
#define LOOM_IOMMU_STRIDE 4096
bool loom_iommu_enabled(void);
void loom_iommu_register(void);
hwaddr loom_iommu_address(unsigned owner);
unsigned loom_iommu_nodes(void);
unsigned loom_pci_device_owner(PCIDevice *dev);

bool loom_pci_enabled(void);
void loom_pci_prepare(void);
void loom_pci_bootstrap(void);
void loom_pci_publish(PCIDevice *dev);
void loom_pci_config_address(uint32_t value);
bool loom_pci_handle_pio(uint16_t port, MemTxAttrs attrs, void *data,
                         int direction, int size, uint32_t count);
bool loom_pci_handle_mmio(hwaddr addr, MemTxAttrs attrs, uint8_t *data,
                          unsigned size, bool write);
void loom_pci_dispatch(const LoomPCIRequest *req, LoomPCIResponse *resp);
int loom_pci_rpc(int owner, const LoomPCIRequest *req, LoomPCIResponse *resp);
bool loom_pci_intx_requires_proxy(PCIDevice *dev);
bool loom_pci_route_intx(PCIDevice *dev, unsigned pin, int change);
void loom_pci_intx_notify(const LoomPCIRequest *req);
void loom_pci_intx_dispatch(const LoomPCIRequest *req);
void loom_pci_set_effective_irq(PCIDevice *dev, unsigned pin, bool level);
void loom_pci_reset_begin(PCIDevice *dev);
void loom_pci_reset_end(PCIDevice *dev);
bool loom_pci_config_will_reset(PCIDevice *dev, uint32_t addr,
                                uint32_t value, unsigned len);

#define LOOM_DIRECTORY_CHUNK 3000
#define LOOM_DIRECTORY_UPDATE_SIZE 448
int loom_pci_directory_push(unsigned peer, uint64_t serial, unsigned device_id,
                            const void *data, unsigned size);
int loom_pci_directory_update_dispatch(unsigned peer, unsigned device_id,
                                       const void *data, unsigned size);
void loom_pci_directory_ack_dispatch(unsigned peer, uint64_t serial,
                                      int status);
int loom_pci_directory_rpc(int owner, uint32_t offset, void *buffer, uint32_t size);
void loom_pci_directory_dispatch(uint32_t offset, void *buffer, uint32_t size,
                                 int *status);
bool loom_pci_forward_msi(MSIMessage msg, int *status);
int loom_pci_inject_msi(MSIMessage msg);

#endif
