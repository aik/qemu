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
#include "linux/vfio.h"
#include "hw/vfio/vfio.h"

static Property spapr_phb_vfio_properties[] = {
    DEFINE_PROP_INT32("iommu", sPAPRPHBVFIOState, iommugroupid, -1),
    DEFINE_PROP_UINT8("levels", sPAPRPHBVFIOState, levels, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static int spapr_phb_vfio_levels(uint32_t entries)
{
    unsigned pages = (entries * sizeof(uint64_t)) / getpagesize();
    int levels;

    if (pages < 8) {
        levels = 1;
    } else if (pages < 64) {
        levels = 2;
    } else if (pages < 4096) {
        levels = 3;
    } else {
        levels = 4;
    }

    return levels;
}

static void spapr_phb_vfio_init_dma_window(sPAPRPHBState *sphb, uint32_t liobn,
                           uint32_t page_shift, uint32_t window_shift_hint,
                           Error **errp)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    int ret;
    uint32_t nb_table;
    sPAPRTCETable *tcet = spapr_tce_find_by_liobn(liobn);
    struct vfio_iommu_spapr_tce_create create = {
        .argsz = sizeof(create),
        .page_shift = page_shift,
        .window_shift = window_shift_hint,
        .levels = svphb->levels,
        .start_addr = 0,
    };

    ret = vfio_container_ioctl(&svphb->phb.iommu_as,
                               VFIO_CHECK_EXTENSION,
                               (void *) VFIO_SPAPR_TCE_IOMMU);
    if (ret != 1) {
        error_setg_errno(errp, -ret,
                         "spapr-vfio: SPAPR extension is not supported");
        return;
    }

    if (!window_shift_hint && !SPAPR_PCI_DMA_WINDOW_NUM(liobn)) {
        struct vfio_iommu_spapr_tce_info info = { .argsz = sizeof(info) };
        ret = vfio_container_ioctl(&sphb->iommu_as,
                                   VFIO_IOMMU_SPAPR_TCE_GET_INFO, &info);
        if (ret) {
            error_setg_errno(errp, -ret,
                             "spapr-vfio: get info from container failed");
            return;
        }

        create.window_shift = up_pow_of_two(info.dma32_window_size);
    }

    if (!create.levels) {
        create.levels = spapr_phb_vfio_levels(1ULL <<
                                              (window_shift_hint - page_shift));
    }

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_CREATE, &create);
    if (ret) {
        error_setg_errno(errp, -ret,
                         "spapr-vfio: create window failed");
        return;
    }

    nb_table = 1ULL << (create.window_shift - page_shift);
    spapr_tce_set_props(tcet, create.start_addr, page_shift, nb_table, true);
    spapr_tce_table_enable(tcet);
}

static int spapr_pci_vfio_ddw_query(sPAPRPHBState *sphb,
                                    uint32_t *windows_supported,
                                    uint32_t *page_size_mask,
                                    uint32_t *dma32_window_size,
                                    uint64_t *dma64_window_size)
{
    struct vfio_iommu_spapr_tce_info info = { .argsz = sizeof(info) };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_GET_INFO, &info);
    if (ret) {
        return ret;
    }

    *windows_supported = info.windows_supported;
    *page_size_mask = info.flags & DDW_PGSIZE_MASK;
    *dma32_window_size = info.dma32_window_size;
    *dma64_window_size = 1ULL << up_pow_of_two(ram_size);

    return ret;
}

static int spapr_pci_vfio_ddw_remove(sPAPRPHBState *sphb, sPAPRTCETable *tcet)
{
    struct vfio_iommu_spapr_tce_remove remove = {
        .argsz = sizeof(remove),
        .start_addr = tcet->bus_offset
    };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as,
                               VFIO_IOMMU_SPAPR_TCE_REMOVE, &remove);

    return ret;
}

static void spapr_phb_vfio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    sPAPRPHBClass *spc = SPAPR_PCI_HOST_BRIDGE_CLASS(klass);

    dc->props = spapr_phb_vfio_properties;
    spc->init_dma_window = spapr_phb_vfio_init_dma_window;
    spc->ddw_query = spapr_pci_vfio_ddw_query;
    spc->ddw_remove = spapr_pci_vfio_ddw_remove;
}

static const TypeInfo spapr_phb_vfio_info = {
    .name          = TYPE_SPAPR_PCI_VFIO_HOST_BRIDGE,
    .parent        = TYPE_SPAPR_PCI_HOST_BRIDGE,
    .instance_size = sizeof(sPAPRPHBVFIOState),
    .class_init    = spapr_phb_vfio_class_init,
    .class_size    = sizeof(sPAPRPHBClass),
};

static void spapr_pci_vfio_register_types(void)
{
    type_register_static(&spapr_phb_vfio_info);
}

type_init(spapr_pci_vfio_register_types)
