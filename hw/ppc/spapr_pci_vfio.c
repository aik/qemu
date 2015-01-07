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
    DEFINE_PROP_END_OF_LIST(),
};

static int spapr_pci_vfio_ddw_query(sPAPRPHBState *sphb,
                                    uint32_t *windows_available,
                                    uint32_t *page_size_mask,
                                    uint32_t *dma32_window_size)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    struct vfio_iommu_spapr_tce_info info = { .argsz = sizeof(info) };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as, svphb->iommugroupid,
                               VFIO_IOMMU_SPAPR_TCE_GET_INFO, &info);
    if (ret) {
        return ret;
    }

    *windows_available = info.windows_supported;
    *page_size_mask = info.flags & DDW_PGSIZE_MASK;
    *dma32_window_size = info.dma32_window_size;

    return ret;
}

static int spapr_pci_vfio_ddw_create(sPAPRPHBState *sphb, uint32_t liobn,
                                     uint32_t page_shift, uint32_t window_shift,
                                     sPAPRTCETable **ptcet)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    struct vfio_iommu_spapr_tce_create create = {
        .argsz = sizeof(create),
        .page_shift = page_shift,
        .window_shift = window_shift,
        .levels = 1,
        .start_addr = 0,
    };
    int ret;

    ret = vfio_container_ioctl(&sphb->iommu_as, svphb->iommugroupid,
                               VFIO_IOMMU_SPAPR_TCE_CREATE, &create);
    if (ret) {
        return ret;
    }

    *ptcet = spapr_tce_new_table(DEVICE(sphb), liobn,
                                 create.start_addr,
                                 page_shift,
                                 1ULL << (window_shift - page_shift),
                                 true);
    if (!*ptcet) {
        return -1;
    }
    memory_region_add_subregion(&sphb->iommu_root, (*ptcet)->bus_offset,
                                spapr_tce_get_iommu(*ptcet));

    ++sphb->windows_num;

    return ret;
}

static int spapr_pci_vfio_ddw_remove(sPAPRPHBState *sphb, sPAPRTCETable *tcet)
{
    sPAPRPHBVFIOState *svphb = SPAPR_PCI_VFIO_HOST_BRIDGE(sphb);
    struct vfio_iommu_spapr_tce_remove remove = {
        .argsz = sizeof(remove),
        .start_addr = tcet->bus_offset
    };
    int ret;

    spapr_pci_ddw_remove(sphb, tcet);
    ret = vfio_container_ioctl(&sphb->iommu_as, svphb->iommugroupid,
                               VFIO_IOMMU_SPAPR_TCE_REMOVE, &remove);

    return ret;
}

static void spapr_phb_vfio_class_init(ObjectClass *klass, void *data)
{
    sPAPRPHBClass *spc = SPAPR_PCI_HOST_BRIDGE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->props = spapr_phb_vfio_properties;
    spc->ddw_query = spapr_pci_vfio_ddw_query;
    spc->ddw_create = spapr_pci_vfio_ddw_create;
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
