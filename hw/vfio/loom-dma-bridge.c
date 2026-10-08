/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "hw/vfio/vfio-common.h"
#include "hw/vfio/loom-dma.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_LOOM_DMA_BRIDGE "loom-dma-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(LoomDMABridge, LOOM_DMA_BRIDGE)

struct LoomDMABridge {
    PCIDevice parent;
    MemoryRegion mmio;
    uint32_t target_devfn, direction;
    int32_t status;
    uint64_t address, length;
};

static VFIOContainerBase *bridge_container(LoomDMABridge *s)
{
    VFIODevice *v;
    QLIST_FOREACH(v, &vfio_device_list, global_next) {
        if (v->type == VFIO_DEVICE_TYPE_PCI) {
            PCIDevice *pci = PCI_DEVICE(v->dev);
            if (pci->devfn == s->target_devfn && pci_bus_num(pci_get_bus(pci)) == 0) {
                return v->bcontainer;
            }
        }
    }
    return NULL;
}

static uint64_t bridge_read(void *opaque, hwaddr addr, unsigned size)
{
    LoomDMABridge *s = opaque;
    switch (addr) {
    case 0x00: return 0x4c444d41; /* LDMA protocol version 1 */
    case 0x04: return s->target_devfn;
    case 0x08: return (uint32_t)s->status;
    case 0x0c: return 1;
    case 0x10: return (uint32_t)s->address;
    case 0x14: return s->address >> 32;
    case 0x18: return (uint32_t)s->length;
    case 0x1c: return s->length >> 32;
    case 0x20: return s->direction;
    case 0x30: return loom_vfio_dma_count(bridge_container(s));
    default: return 0;
    }
}

static void bridge_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    LoomDMABridge *s = opaque;
    switch (addr) {
    case 0x10: s->address = (s->address & ~UINT64_C(0xffffffff)) | value; break;
    case 0x14: s->address = (uint32_t)s->address | (value << 32); break;
    case 0x18: s->length = (s->length & ~UINT64_C(0xffffffff)) | value; break;
    case 0x1c: s->length = (uint32_t)s->length | (value << 32); break;
    case 0x20: s->direction = value; break;
    case 0x24:
        s->status = loom_vfio_dma_command(bridge_container(s), value,
                                         s->address, s->length, s->direction);
        break;
    }
}

static const MemoryRegionOps bridge_ops = {
    .read = bridge_read, .write = bridge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void bridge_realize(PCIDevice *pdev, Error **errp)
{
    LoomDMABridge *s = LOOM_DMA_BRIDGE(pdev);
    if (!loom_vfio_dma_enabled() || s->target_devfn > 255) {
        error_setg(errp, "loom-dma-bridge requires DSM DMA mode and a root-bus target");
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(s), &bridge_ops, s,
                          "loom-dma-bridge", 4096);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static Property bridge_properties[] = {
    DEFINE_PROP_UINT32("target-devfn", LoomDMABridge, target_devfn, 0x38),
    DEFINE_PROP_END_OF_LIST(),
};

static void bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = bridge_realize;
    pc->vendor_id = 0x1234;
    pc->device_id = 0x11eb;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_MEMORY_RAM;
    dc->desc = "DSM DMA buffer registration for native VFIO drivers";
    device_class_set_props(dc, bridge_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo bridge_info = {
    .name = TYPE_LOOM_DMA_BRIDGE,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(LoomDMABridge),
    .class_init = bridge_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { },
    },
};
static void bridge_register_types(void)
{
    type_register_static(&bridge_info);
}
type_init(bridge_register_types)
