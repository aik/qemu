#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "hw/vfio/vfio.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/hw.h"
#include "qemu/error-report.h"
#include "sysemu/kvm.h"
#include "trace.h"

static void vfio_iommu_map_notify(Notifier *n, void *data)
{
    VFIOGuestIOMMU *giommu = container_of(n, VFIOGuestIOMMU, n);
    VFIOContainer *container = giommu->container;
    IOMMUTLBEntry *iotlb = data;
    MemoryRegion *mr;
    hwaddr xlat;
    hwaddr len = iotlb->addr_mask + 1;
    void *vaddr;
    int ret;

    trace_vfio_iommu_map_notify(iotlb->iova,
                                iotlb->iova + iotlb->addr_mask);

    /*
     * The IOMMU TLB entry we have just covers translation through
     * this IOMMU to its immediate target.  We need to translate
     * it the rest of the way through to memory.
     */
    mr = address_space_translate(&address_space_memory,
                                 iotlb->translated_addr,
                                 &xlat, &len, iotlb->perm & IOMMU_WO);
    if (!memory_region_is_ram(mr)) {
        error_report("iommu map to non memory area %"HWADDR_PRIx"\n",
                     xlat);
        return;
    }
    /*
     * Translation truncates length to the IOMMU page size,
     * check that it did not truncate too much.
     */
    if (len & iotlb->addr_mask) {
        error_report("iommu has granularity incompatible with target AS\n");
        return;
    }

    if ((iotlb->perm & IOMMU_RW) != IOMMU_NONE) {
        vaddr = memory_region_get_ram_ptr(mr) + xlat;
        ret = vfio_dma_map(container, iotlb->iova,
                           iotlb->addr_mask + 1, vaddr,
                           !(iotlb->perm & IOMMU_WO) || mr->readonly);
        if (ret) {
            error_report("vfio_dma_map(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx", %p) = %d (%m)",
                         container, iotlb->iova,
                         iotlb->addr_mask + 1, vaddr, ret);
        }
    } else {
        ret = vfio_dma_unmap(container, iotlb->iova, iotlb->addr_mask + 1);
        if (ret) {
            error_report("vfio_dma_unmap(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx") = %d (%m)",
                         container, iotlb->iova,
                         iotlb->addr_mask + 1, ret);
        }
    }
}

static void vfio_listener_region_add(MemoryListener *listener,
                                     MemoryRegionSection *section)
{
    VFIOContainer *container = container_of(listener, VFIOContainer,
                                            iommu_data.type1.listener);
    hwaddr iova;
    Int128 llend;
    VFIOGuestIOMMU *giommu;

    if (vfio_listener_skipped_section(section)) {
        trace_vfio_listener_region_add_skip(
            section->offset_within_address_space,
            section->offset_within_address_space +
            int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~TARGET_PAGE_MASK) !=
                 (section->offset_within_region & ~TARGET_PAGE_MASK))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    iova = TARGET_PAGE_ALIGN(section->offset_within_address_space);
    llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(TARGET_PAGE_MASK));

    if (int128_ge(int128_make64(iova), llend)) {
        return;
    }

    memory_region_ref(section->mr);

    trace_vfio_listener_region_add_iommu(iova,
         int128_get64(int128_sub(llend, int128_one())));
    /*
     * FIXME: We should do some checking to see if the
     * capabilities of the host VFIO IOMMU are adequate to model
     * the guest IOMMU
     *
     * FIXME: For VFIO iommu types which have KVM acceleration to
     * avoid bouncing all map/unmaps through qemu this way, this
     * would be the right place to wire that up (tell the KVM
     * device emulation the VFIO iommu handles to use).
     */
    /*
     * This assumes that the guest IOMMU is empty of
     * mappings at this point.
     *
     * One way of doing this is:
     * 1. Avoid sharing IOMMUs between emulated devices or different
     * IOMMU groups.
     * 2. Implement VFIO_IOMMU_ENABLE in the host kernel to fail if
     * there are some mappings in IOMMU.
     *
     * VFIO on SPAPR does that. Other IOMMU models may do that different,
     * they must make sure there are no existing mappings or
     * loop through existing mappings to map them into VFIO.
     */
    giommu = g_malloc0(sizeof(*giommu));
    giommu->iommu = section->mr;
    giommu->container = container;
    giommu->n.notify = vfio_iommu_map_notify;
    QLIST_INSERT_HEAD(&container->giommu_list, giommu, giommu_next);
    memory_region_register_iommu_notifier(giommu->iommu, &giommu->n);
}

static void vfio_listener_region_del(MemoryListener *listener,
                                     MemoryRegionSection *section)
{
    VFIOContainer *container = container_of(listener, VFIOContainer,
                                            iommu_data.type1.listener);
    hwaddr iova, end;
    int ret;
    VFIOGuestIOMMU *giommu;

    if (vfio_listener_skipped_section(section)) {
        trace_vfio_listener_region_del_skip(
            section->offset_within_address_space,
            section->offset_within_address_space +
            int128_get64(int128_sub(section->size, int128_one())));
        return;
    }

    if (unlikely((section->offset_within_address_space & ~TARGET_PAGE_MASK) !=
                 (section->offset_within_region & ~TARGET_PAGE_MASK))) {
        error_report("%s received unaligned region", __func__);
        return;
    }

    QLIST_FOREACH(giommu, &container->giommu_list, giommu_next) {
        if (giommu->iommu == section->mr) {
            memory_region_unregister_iommu_notifier(&giommu->n);
            QLIST_REMOVE(giommu, giommu_next);
            g_free(giommu);
            break;
        }
    }

    /*
     * FIXME: We assume the one big unmap below is adequate to
     * remove any individual page mappings in the IOMMU which
     * might have been copied into VFIO. This works for a page table
     * based IOMMU where a big unmap flattens a large range of IO-PTEs.
     * That may not be true for all IOMMU types.
     */

    iova = TARGET_PAGE_ALIGN(section->offset_within_address_space);
    end = (section->offset_within_address_space + int128_get64(section->size)) &
        TARGET_PAGE_MASK;

    if (iova >= end) {
        return;
    }

    trace_vfio_listener_region_del(iova, end - 1);

    ret = vfio_dma_unmap(container, iova, end - iova);
    memory_region_unref(section->mr);
    if (ret) {
        error_report("vfio_dma_unmap(%p, 0x%"HWADDR_PRIx", "
                     "0x%"HWADDR_PRIx") = %d (%m)",
                     container, iova, end - iova, ret);
    }
}

static const MemoryListener vfio_memory_listener = {
    .region_add = vfio_listener_region_add,
    .region_del = vfio_listener_region_del,
};

static void vfio_listener_release(VFIOContainer *container)
{
    memory_listener_unregister(&container->iommu_data.spapr.listener);
}

void spapr_memory_listener_register(VFIOContainer *container)
{
    container->iommu_data.spapr.listener = vfio_memory_listener;
    container->iommu_data.release = vfio_listener_release;

    memory_listener_register(&container->iommu_data.spapr.listener,
                             container->space->as);
}
