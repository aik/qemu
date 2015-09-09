/*
 * DMA memory preregistration
 *
 * Authors:
 *  Alexey Kardashevskiy <aik@ozlabs.ru>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "hw/vfio/vfio.h"
#include "qemu/error-report.h"
#include "trace.h"

static bool vfio_prereg_listener_skipped_section(MemoryRegionSection *section)
{
    return (!memory_region_is_ram(section->mr) &&
            !memory_region_is_iommu(section->mr)) ||
            memory_region_is_skip_dump(section->mr);
}

static void vfio_prereg_listener_region_add(MemoryListener *listener,
                                            MemoryRegionSection *section)
{
    VFIOMemoryListener *vlistener = container_of(listener, VFIOMemoryListener,
                                                 listener);
    VFIOContainer *container = vlistener->container;
    hwaddr iova;
    Int128 llend;
    int ret;
    hwaddr page_mask = vfio_iommu_page_mask(section->mr);
    struct vfio_iommu_spapr_register_memory reg = {
        .argsz = sizeof(reg),
        .flags = 0,
    };

    if (vfio_prereg_listener_skipped_section(section)) {
        trace_vfio_listener_region_add_skip(
                section->offset_within_address_space,
                section->offset_within_address_space +
                int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~page_mask) !=
                 (section->offset_within_region & ~page_mask))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    iova = ROUND_UP(section->offset_within_address_space, ~page_mask + 1);
    llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(page_mask));

    if (int128_ge(int128_make64(iova), llend)) {
        return;
    }

    memory_region_ref(section->mr);

    /* Here we assume that memory_region_is_ram(section->mr)==true */

    reg.vaddr = (__u64) memory_region_get_ram_ptr(section->mr) +
        section->offset_within_region +
        (iova - section->offset_within_address_space);
    reg.size = int128_get64(llend) - iova;

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_REGISTER_MEMORY, &reg);
    trace_vfio_ram_register(reg.vaddr, reg.size, ret ? -errno : 0);
    if (ret) {
        /*
         * On the initfn path, store the first error in the container so we
         * can gracefully fail.  Runtime, there's not much we can do other
         * than throw a hardware error.
         */
        if (!container->initialized) {
            if (!container->error) {
                container->error = ret;
            }
        } else {
            hw_error("vfio: DMA mapping failed, unable to continue");
        }
    }
}

static void vfio_prereg_listener_region_del(MemoryListener *listener,
                                            MemoryRegionSection *section)
{
    VFIOMemoryListener *vlistener = container_of(listener, VFIOMemoryListener,
                                                 listener);
    VFIOContainer *container = vlistener->container;
    hwaddr iova, end;
    int ret;
    hwaddr page_mask = vfio_iommu_page_mask(section->mr);
    struct vfio_iommu_spapr_register_memory reg = {
        .argsz = sizeof(reg),
        .flags = 0,
    };

    if (vfio_prereg_listener_skipped_section(section)) {
        trace_vfio_listener_region_del_skip(
                section->offset_within_address_space,
                section->offset_within_address_space +
                int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~page_mask) !=
                 (section->offset_within_region & ~page_mask))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    iova = ROUND_UP(section->offset_within_address_space, ~page_mask + 1);
    end = (section->offset_within_address_space + int128_get64(section->size)) &
        page_mask;

    if (iova >= end) {
        return;
    }

    reg.vaddr = (__u64) memory_region_get_ram_ptr(section->mr) +
        section->offset_within_region +
        (iova - section->offset_within_address_space);
    reg.size = end - iova;

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_UNREGISTER_MEMORY, &reg);
    trace_vfio_ram_unregister(reg.vaddr, reg.size, ret ? -errno : 0);
}

const MemoryListener vfio_prereg_listener = {
    .region_add = vfio_prereg_listener_region_add,
    .region_del = vfio_prereg_listener_region_del,
};
