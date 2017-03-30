/*
 * DMA memory preregistration
 *
 * Authors:
 *  Alexey Kardashevskiy <aik@ozlabs.ru>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "hw/hw.h"
#include "hw/ppc/spapr.h"
#include "qemu/error-report.h"
#include "trace.h"
#ifdef CONFIG_KVM
#include "linux/kvm.h"
#endif

static bool vfio_prereg_listener_skipped_section(MemoryRegionSection *section)
{
    if (memory_region_is_iommu(section->mr)) {
        hw_error("Cannot possibly preregister IOMMU memory");
    }

    return !memory_region_is_ram(section->mr) ||
            memory_region_is_ram_device(section->mr);
}

static void *vfio_prereg_gpa_to_vaddr(MemoryRegionSection *section, hwaddr gpa)
{
    return memory_region_get_ram_ptr(section->mr) +
        section->offset_within_region +
        (gpa - section->offset_within_address_space);
}

static void vfio_prereg_listener_region_add(MemoryListener *listener,
                                            MemoryRegionSection *section)
{
    VFIOContainer *container = container_of(listener, VFIOContainer,
                                            prereg_listener);
    const hwaddr gpa = section->offset_within_address_space;
    hwaddr end;
    int ret;
    hwaddr page_mask = qemu_real_host_page_mask;
    struct vfio_iommu_spapr_register_memory reg = {
        .argsz = sizeof(reg),
        .flags = 0,
    };

    if (vfio_prereg_listener_skipped_section(section)) {
        trace_vfio_prereg_listener_region_add_skip(
                section->offset_within_address_space,
                section->offset_within_address_space +
                int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~page_mask) ||
                 (section->offset_within_region & ~page_mask) ||
                 (int128_get64(section->size) & ~page_mask))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    end = section->offset_within_address_space + int128_get64(section->size);
    if (gpa >= end) {
        return;
    }

    memory_region_ref(section->mr);

    reg.vaddr = (uintptr_t) vfio_prereg_gpa_to_vaddr(section, gpa);
    reg.size = end - gpa;

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_REGISTER_MEMORY, &reg);
    trace_vfio_prereg_register(reg.vaddr, reg.size, ret ? -errno : 0);
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
            hw_error("vfio: Memory registering failed, unable to continue");
        }
    }
}

static void vfio_prereg_listener_region_del(MemoryListener *listener,
                                            MemoryRegionSection *section)
{
    VFIOContainer *container = container_of(listener, VFIOContainer,
                                            prereg_listener);
    const hwaddr gpa = section->offset_within_address_space;
    hwaddr end;
    int ret;
    hwaddr page_mask = qemu_real_host_page_mask;
    struct vfio_iommu_spapr_register_memory reg = {
        .argsz = sizeof(reg),
        .flags = 0,
    };

    if (vfio_prereg_listener_skipped_section(section)) {
        trace_vfio_prereg_listener_region_del_skip(
                section->offset_within_address_space,
                section->offset_within_address_space +
                int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~page_mask) ||
                 (section->offset_within_region & ~page_mask) ||
                 (int128_get64(section->size) & ~page_mask))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    end = section->offset_within_address_space + int128_get64(section->size);
    if (gpa >= end) {
        return;
    }

    reg.vaddr = (uintptr_t) vfio_prereg_gpa_to_vaddr(section, gpa);
    reg.size = end - gpa;

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_UNREGISTER_MEMORY, &reg);
    trace_vfio_prereg_unregister(reg.vaddr, reg.size, ret ? -errno : 0);
}

const MemoryListener vfio_prereg_listener = {
    .region_add = vfio_prereg_listener_region_add,
    .region_del = vfio_prereg_listener_region_del,
};

int vfio_spapr_create_window(VFIOContainer *container,
                             MemoryRegionSection *section,
                             hwaddr *pgsize)
{
    int ret;
    IOMMUMemoryRegion *iommu_mr = IOMMU_MEMORY_REGION(section->mr);
    unsigned pagesize = memory_region_iommu_get_min_page_size(iommu_mr);
    unsigned entries, pages;
    struct vfio_iommu_spapr_tce_create create = { .argsz = sizeof(create) };

    /*
     * FIXME: For VFIO iommu types which have KVM acceleration to
     * avoid bouncing all map/unmaps through qemu this way, this
     * would be the right place to wire that up (tell the KVM
     * device emulation the VFIO iommu handles to use).
     */
    create.window_size = int128_get64(section->size);
    create.page_shift = ctz64(pagesize);
    /*
     * SPAPR host supports multilevel TCE tables, there is some
     * heuristic to decide how many levels we want for our table:
     * 0..64 = 1; 65..4096 = 2; 4097..262144 = 3; 262145.. = 4
     */
    entries = create.window_size >> create.page_shift;
    pages = MAX((entries * sizeof(uint64_t)) / getpagesize(), 1);
    pages = MAX(pow2ceil(pages) - 1, 1); /* Round up */
    create.levels = ctz64(pages) / 6 + 1;

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_TCE_CREATE, &create);
    if (ret) {
        error_report("Failed to create a window, ret = %d (%m)", ret);
        return -errno;
    }

    if (create.start_addr != section->offset_within_address_space) {
        vfio_spapr_remove_window(container, create.start_addr);

        error_report("Host doesn't support DMA window at %"HWADDR_PRIx", must be %"PRIx64,
                     section->offset_within_address_space,
                     (uint64_t)create.start_addr);
        return -EINVAL;
    }
    trace_vfio_spapr_create_window(create.page_shift,
                                   create.window_size,
                                   create.start_addr);
    *pgsize = pagesize;

    return 0;
}

int vfio_spapr_notify_kvm(int vfio_kvm_device_fd, int groupfd,
                          IOMMUMemoryRegion *iommu_mr)
{
#ifdef CONFIG_KVM
    struct kvm_vfio_spapr_tce param = {
        .groupfd = groupfd,
    };
    struct kvm_device_attr attr = {
        .group = KVM_DEV_VFIO_GROUP,
        .attr = KVM_DEV_VFIO_GROUP_SET_SPAPR_TCE,
        .addr = (uint64_t)(unsigned long)&param,
    };
    IOMMUMemoryRegion *spapr_iommu_mr = SPAPR_IOMMU_MEMORY_REGION(iommu_mr);
    sPAPRIOMMUMemoryRegionClass *simrc =
            SPAPR_IOMMU_MEMORY_REGION_GET_CLASS(spapr_iommu_mr);

    if (!simrc->get_fd) {
        error_report("vfio: No get_fd defined for IOMMU MR");
        return -EFAULT;
    }

    param.tablefd = simrc->get_fd(spapr_iommu_mr);

    if (param.tablefd != -1) {
            printf("+++Q+++ (%u) %s %u\n", getpid(), __func__, __LINE__);
        if (ioctl(vfio_kvm_device_fd, KVM_SET_DEVICE_ATTR, &attr)) {
            error_report("vfio: failed to setup fd %d for a group with fd %d: %s",
                         param.tablefd, param.groupfd, strerror(errno));
            return -errno;
        }
    }
    trace_vfio_spapr_notify_kvm(groupfd, param.tablefd);
#endif
    return 0;
}

int vfio_spapr_remove_window(VFIOContainer *container,
                             hwaddr offset_within_address_space)
{
    struct vfio_iommu_spapr_tce_remove remove = {
        .argsz = sizeof(remove),
        .start_addr = offset_within_address_space,
    };
    int ret;

#if 0//def CONFIG_KVM
    printf("+++Q+++ (%u) %s %u\n", getpid(), __func__, __LINE__);
    if (kvm_enabled() && section && section->mr->iommu_ops->get_fd) {
        int fd = section->mr->iommu_ops->get_fd(section->mr);
        struct kvm_spapr_tce_vfio param = {
            .argsz = sizeof(param),
            .flags = 0,
            .container_fd = container->fd,
        };

        printf("+++Q+++ (%u) %s %u UNSET\n", getpid(), __func__, __LINE__);
        if (ioctl(fd, KVM_SPAPR_TCE_VFIO_UNSET, &param)) {
            error_report("vfio: failed to setup fd %d for a group: %s",
                         fd, strerror(errno));
        }
    }
#endif
    printf("+++Q+++ (%u) %s %u\n", getpid(), __func__, __LINE__);

    ret = ioctl(container->fd, VFIO_IOMMU_SPAPR_TCE_REMOVE, &remove);
    if (ret) {
        error_report("Failed to remove window at %"PRIx64,
                     (uint64_t)remove.start_addr);
        return -errno;
    }

    trace_vfio_spapr_remove_window(offset_within_address_space);

    return 0;
}
