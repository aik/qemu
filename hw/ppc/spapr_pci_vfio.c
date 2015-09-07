/*
 * QEMU sPAPR PCI host for VFIO
 *
 * Copyright (c) 2011-2014 Alexey Kardashevskiy, IBM Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License,
 *  or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/ppc/spapr.h"
#include "hw/pci-host/spapr.h"
#include "hw/pci/msix.h"
#include "hw/vfio/vfio.h"

#ifdef CONFIG_LINUX
#include "linux/vfio.h"
#include "trace.h"

static Property spapr_phb_vfio_properties[] = {
    DEFINE_PROP_INT32("iommu", sPAPRPHBState, iommugroupid, -1),
    DEFINE_PROP_END_OF_LIST(),
};

int spapr_phb_vfio_dma_capabilities_update(sPAPRPHBState *sphb)
{
    struct vfio_iommu_spapr_tce_info info = { .argsz = sizeof(info) };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_GET_INFO, &info);
    if (ret) {
        return ret;
    }

    sphb->dma32_window_start = info.dma32_window_start;
    sphb->dma32_window_size = info.dma32_window_size;

    if (sphb->ddw_enabled && (info.flags & VFIO_IOMMU_SPAPR_INFO_DDW)) {
        sphb->windows_supported = info.ddw.max_dynamic_windows_supported;
        sphb->page_size_mask = info.ddw.pgsizes;
        sphb->dma64_window_size = pow2ceil(ram_size);
        sphb->max_levels = info.ddw.levels;
    } else {
        /* If VFIO_IOMMU_INFO_DDW is not set, disable DDW */
        sphb->ddw_enabled = false;
    }

    return ret;
}

static int spapr_phb_vfio_levels(uint32_t entries)
{
    unsigned pages = (entries * sizeof(uint64_t)) / getpagesize();
    int levels;

    if (pages <= 64) {
        levels = 1;
    } else if (pages <= 64*64) {
        levels = 2;
    } else if (pages <= 64*64*64) {
        levels = 3;
    } else {
        levels = 4;
    }

    return levels;
}

int spapr_phb_vfio_dma_init_window(sPAPRPHBState *sphb,
                                   uint32_t page_shift,
                                   uint64_t window_size,
                                   uint64_t *bus_offset)
{
    int ret;
    struct vfio_iommu_spapr_tce_create create = {
        .argsz = sizeof(create),
        .page_shift = page_shift,
        .window_size = window_size,
        .levels = sphb->levels,
        .start_addr = 0,
    };

    /*
     * Dynamic windows are supported, that means that there is no
     * pre-created window and we have to create one.
     */
    if (!create.levels) {
        create.levels = spapr_phb_vfio_levels(create.window_size >>
                                              page_shift);
    }

    if (create.levels > sphb->max_levels) {
        return -EINVAL;
    }

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_CREATE, &create);
    if (ret) {
        return ret;
    }
    *bus_offset = create.start_addr;

    trace_spapr_pci_vfio_init_window(page_shift, window_size, *bus_offset);

    return 0;
}

int spapr_phb_vfio_dma_remove_window(sPAPRPHBState *sphb, uint64_t bus_offset)
{
    struct vfio_iommu_spapr_tce_remove remove = {
        .argsz = sizeof(remove),
        .start_addr = bus_offset
    };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_REMOVE, &remove);
    if (ret) {
        return ret;
    }

    trace_spapr_pci_vfio_remove_window(bus_offset);

    return ret;
}

void spapr_phb_vfio_eeh_reenable(sPAPRPHBState *sphb)
{
    struct vfio_eeh_pe_op op = {
        .argsz = sizeof(op),
        .op    = VFIO_EEH_PE_ENABLE
    };

    vfio_container_ioctl(&sphb->iommu_as, VFIO_EEH_PE_OP, &op);
}

int spapr_phb_vfio_eeh_set_option(sPAPRPHBState *sphb,
                                  PCIDevice *pdev, int option)
{
    struct vfio_eeh_pe_op op = { .argsz = sizeof(op) };
    int ret;

    switch (option) {
    case RTAS_EEH_DISABLE:
        op.op = VFIO_EEH_PE_DISABLE;
        break;
    case RTAS_EEH_ENABLE:
        op.op = VFIO_EEH_PE_ENABLE;
        break;
    case RTAS_EEH_THAW_IO:
        op.op = VFIO_EEH_PE_UNFREEZE_IO;
        break;
    case RTAS_EEH_THAW_DMA:
        op.op = VFIO_EEH_PE_UNFREEZE_DMA;
        break;
    default:
        return RTAS_OUT_PARAM_ERROR;
    }

    ret = vfio_container_ioctl(&sphb->iommu_as, VFIO_EEH_PE_OP, &op);
    if (ret < 0) {
        return RTAS_OUT_HW_ERROR;
    }

    return RTAS_OUT_SUCCESS;
}

int spapr_phb_vfio_eeh_get_state(sPAPRPHBState *sphb, int *state)
{
    struct vfio_eeh_pe_op op = { .argsz = sizeof(op) };
    int ret;

    op.op = VFIO_EEH_PE_GET_STATE;
    ret = vfio_container_ioctl(&sphb->iommu_as, VFIO_EEH_PE_OP, &op);
    if (ret < 0) {
        return RTAS_OUT_PARAM_ERROR;
    }

    *state = ret;
    return RTAS_OUT_SUCCESS;
}

static void spapr_phb_vfio_eeh_clear_dev_msix(PCIBus *bus,
                                              PCIDevice *pdev,
                                              void *opaque)
{
    /* Check if the device is VFIO PCI device */
    if (!object_dynamic_cast(OBJECT(pdev), "vfio-pci")) {
        return;
    }

    /*
     * The MSIx table will be cleaned out by reset. We need
     * disable it so that it can be reenabled properly. Also,
     * the cached MSIx table should be cleared as it's not
     * reflecting the contents in hardware.
     */
    if (msix_enabled(pdev)) {
        uint16_t flags;

        flags = pci_host_config_read_common(pdev,
                                            pdev->msix_cap + PCI_MSIX_FLAGS,
                                            pci_config_size(pdev), 2);
        flags &= ~PCI_MSIX_FLAGS_ENABLE;
        pci_host_config_write_common(pdev,
                                     pdev->msix_cap + PCI_MSIX_FLAGS,
                                     pci_config_size(pdev), flags, 2);
    }

    msix_reset(pdev);
}

static void spapr_phb_vfio_eeh_clear_bus_msix(PCIBus *bus, void *opaque)
{
       pci_for_each_device(bus, pci_bus_num(bus),
                           spapr_phb_vfio_eeh_clear_dev_msix, NULL);
}

static void spapr_phb_vfio_eeh_pre_reset(sPAPRPHBState *sphb)
{
       PCIHostState *phb = PCI_HOST_BRIDGE(sphb);

       pci_for_each_bus(phb->bus, spapr_phb_vfio_eeh_clear_bus_msix, NULL);
}

int spapr_phb_vfio_eeh_reset(sPAPRPHBState *sphb, int option)
{
    struct vfio_eeh_pe_op op = { .argsz = sizeof(op) };
    int ret;

    switch (option) {
    case RTAS_SLOT_RESET_DEACTIVATE:
        op.op = VFIO_EEH_PE_RESET_DEACTIVATE;
        break;
    case RTAS_SLOT_RESET_HOT:
        spapr_phb_vfio_eeh_pre_reset(sphb);
        op.op = VFIO_EEH_PE_RESET_HOT;
        break;
    case RTAS_SLOT_RESET_FUNDAMENTAL:
        spapr_phb_vfio_eeh_pre_reset(sphb);
        op.op = VFIO_EEH_PE_RESET_FUNDAMENTAL;
        break;
    default:
        return RTAS_OUT_PARAM_ERROR;
    }

    ret = vfio_container_ioctl(&sphb->iommu_as, VFIO_EEH_PE_OP, &op);
    if (ret < 0) {
        return RTAS_OUT_HW_ERROR;
    }

    return RTAS_OUT_SUCCESS;
}

int spapr_phb_vfio_eeh_configure(sPAPRPHBState *sphb)
{
    struct vfio_eeh_pe_op op = { .argsz = sizeof(op) };
    int ret;

    op.op = VFIO_EEH_PE_CONFIGURE;
    ret = vfio_container_ioctl(&sphb->iommu_as, VFIO_EEH_PE_OP, &op);
    if (ret < 0) {
        return RTAS_OUT_PARAM_ERROR;
    }

    return RTAS_OUT_SUCCESS;
}

static void spapr_phb_vfio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = spapr_phb_vfio_properties;
}

static const TypeInfo spapr_phb_vfio_info = {
    .name          = TYPE_SPAPR_PCI_VFIO_HOST_BRIDGE,
    .parent        = TYPE_SPAPR_PCI_HOST_BRIDGE,
    .class_init    = spapr_phb_vfio_class_init,
};

static void spapr_pci_vfio_register_types(void)
{
    type_register_static(&spapr_phb_vfio_info);
}

type_init(spapr_pci_vfio_register_types)

#else /* !CONFIG_LINUX */

int spapr_phb_vfio_dma_capabilities_update(sPAPRPHBState *sphb)
{
    return -EINVAL;
}

int spapr_phb_vfio_eeh_set_option(sPAPRPHBState *sphb,
                                  PCIDevice *pdev, int option)
{
    return RTAS_OUT_HW_ERROR;
}

int spapr_phb_vfio_eeh_get_state(sPAPRPHBState *sphb, int *state)
{
    return RTAS_OUT_HW_ERROR;
}

int spapr_phb_vfio_eeh_reset(sPAPRPHBState *sphb, int option)
{
    return RTAS_OUT_HW_ERROR;
}

int spapr_phb_vfio_eeh_configure(sPAPRPHBState *sphb)
{
    return RTAS_OUT_HW_ERROR;
}

int spapr_phb_vfio_dma_init_window(sPAPRPHBState *sphb,
                                   uint32_t page_shift,
                                   uint64_t window_size,
                                   uint64_t *bus_offset)
{
    return -1;
}

int spapr_phb_vfio_dma_remove_window(sPAPRPHBState *sphb, uint64_t bus_offset)
{
    return -1;
}

void spapr_phb_vfio_eeh_reenable(sPAPRPHBState *sphb)
{
}
#endif
