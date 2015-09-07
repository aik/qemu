/*
 * QEMU sPAPR Dynamic DMA windows support
 *
 * Copyright (c) 2015 Alexey Kardashevskiy, IBM Corporation.
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

#include "qemu/error-report.h"
#include "hw/ppc/spapr.h"
#include "hw/pci-host/spapr.h"
#include "trace.h"

static int spapr_phb_get_active_win_num_cb(Object *child, void *opaque)
{
    sPAPRTCETable *tcet;

    tcet = (sPAPRTCETable *) object_dynamic_cast(child, TYPE_SPAPR_TCE_TABLE);
    if (tcet && tcet->enabled) {
        ++*(unsigned *)opaque;
    }
    return 0;
}

static unsigned spapr_phb_get_active_win_num(sPAPRPHBState *sphb)
{
    unsigned ret = 0;

    object_child_foreach(OBJECT(sphb), spapr_phb_get_active_win_num_cb, &ret);

    return ret;
}

static int spapr_phb_get_free_liobn_cb(Object *child, void *opaque)
{
    sPAPRTCETable *tcet;

    tcet = (sPAPRTCETable *) object_dynamic_cast(child, TYPE_SPAPR_TCE_TABLE);
    if (tcet && !tcet->enabled) {
        *(uint32_t *)opaque = tcet->liobn;
        return 1;
    }
    return 0;
}

static unsigned spapr_phb_get_free_liobn(sPAPRPHBState *sphb)
{
    uint32_t liobn = 0;

    object_child_foreach(OBJECT(sphb), spapr_phb_get_free_liobn_cb, &liobn);

    return liobn;
}

static uint32_t spapr_query_mask(struct ppc_one_seg_page_size *sps,
                                 uint64_t page_mask)
{
    int i, j;
    uint32_t mask = 0;
    const struct { int shift; uint32_t mask; } masks[] = {
        { 12, RTAS_DDW_PGSIZE_4K },
        { 16, RTAS_DDW_PGSIZE_64K },
        { 24, RTAS_DDW_PGSIZE_16M },
        { 25, RTAS_DDW_PGSIZE_32M },
        { 26, RTAS_DDW_PGSIZE_64M },
        { 27, RTAS_DDW_PGSIZE_128M },
        { 28, RTAS_DDW_PGSIZE_256M },
        { 34, RTAS_DDW_PGSIZE_16G },
    };

    for (i = 0; i < PPC_PAGE_SIZES_MAX_SZ; i++) {
        for (j = 0; j < ARRAY_SIZE(masks); ++j) {
            if ((sps[i].page_shift == masks[j].shift) &&
                    (page_mask & (1ULL << masks[j].shift))) {
                mask |= masks[j].mask;
            }
        }
    }

    return mask;
}

static void rtas_ibm_query_pe_dma_window(PowerPCCPU *cpu,
                                         sPAPRMachineState *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args,
                                         uint32_t nret, target_ulong rets)
{
    CPUPPCState *env = &cpu->env;
    sPAPRPHBState *sphb;
    uint64_t buid;
    uint32_t avail, addr, pgmask = 0;
    unsigned current;

    if ((nargs != 3) || (nret != 5)) {
        goto param_error_exit;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    addr = rtas_ld(args, 0);
    sphb = spapr_pci_find_phb(spapr, buid);
    if (!sphb || !sphb->ddw_enabled) {
        goto param_error_exit;
    }

    current = spapr_phb_get_active_win_num(sphb);
    avail = (sphb->windows_supported > current) ?
            (sphb->windows_supported - current) : 0;

    /* Work out supported page masks */
    pgmask = spapr_query_mask(env->sps.sps, sphb->page_size_mask);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, avail);

    /*
     * This is "Largest contiguous block of TCEs allocated specifically
     * for (that is, are reserved for) this PE".
     * Return the maximum number as all RAM was in 4K pages.
     */
    rtas_st(rets, 2, sphb->dma64_window_size >> SPAPR_TCE_PAGE_SHIFT);
    rtas_st(rets, 3, pgmask);
    rtas_st(rets, 4, 0); /* DMA migration mask, not supported */

    trace_spapr_iommu_ddw_query(buid, addr, avail, sphb->dma64_window_size,
                                pgmask);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void rtas_ibm_create_pe_dma_window(PowerPCCPU *cpu,
                                          sPAPRMachineState *spapr,
                                          uint32_t token, uint32_t nargs,
                                          target_ulong args,
                                          uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    sPAPRTCETable *tcet = NULL;
    uint32_t addr, page_shift, window_shift, liobn;
    uint64_t buid;
    long ret;

    if ((nargs != 5) || (nret != 4)) {
        goto param_error_exit;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    addr = rtas_ld(args, 0);
    sphb = spapr_pci_find_phb(spapr, buid);
    if (!sphb || !sphb->ddw_enabled) {
        goto param_error_exit;
    }

    page_shift = rtas_ld(args, 3);
    window_shift = rtas_ld(args, 4);
    liobn = spapr_phb_get_free_liobn(sphb);

    if (!liobn || !(sphb->page_size_mask & (1ULL << page_shift))) {
        goto hw_error_exit;
    }

    if (window_shift < page_shift) {
        goto param_error_exit;
    }

    ret = spapr_phb_dma_init_window(sphb, liobn, page_shift,
                                    1ULL << window_shift);
    tcet = spapr_tce_find_by_liobn(liobn);
    trace_spapr_iommu_ddw_create(buid, addr, 1ULL << page_shift,
                                 1ULL << window_shift,
                                 tcet ? tcet->bus_offset : 0xbaadf00d,
                                 liobn, ret);
    if (ret || !tcet) {
        goto hw_error_exit;
    }

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, liobn);
    rtas_st(rets, 2, tcet->bus_offset >> 32);
    rtas_st(rets, 3, tcet->bus_offset & ((uint32_t) -1));

    return;

hw_error_exit:
    rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void rtas_ibm_remove_pe_dma_window(PowerPCCPU *cpu,
                                          sPAPRMachineState *spapr,
                                          uint32_t token, uint32_t nargs,
                                          target_ulong args,
                                          uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    sPAPRTCETable *tcet;
    uint32_t liobn;
    long ret;

    if ((nargs != 1) || (nret != 1)) {
        goto param_error_exit;
    }

    liobn = rtas_ld(args, 0);
    tcet = spapr_tce_find_by_liobn(liobn);
    if (!tcet) {
        goto param_error_exit;
    }

    sphb = SPAPR_PCI_HOST_BRIDGE(OBJECT(tcet)->parent);
    if (!sphb || !sphb->ddw_enabled) {
        goto param_error_exit;
    }

    ret = spapr_phb_dma_remove_window(sphb, tcet);
    trace_spapr_iommu_ddw_remove(liobn, ret);
    if (ret) {
        goto hw_error_exit;
    }

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    return;

hw_error_exit:
    rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void rtas_ibm_reset_pe_dma_window(PowerPCCPU *cpu,
                                         sPAPRMachineState *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args,
                                         uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    uint64_t buid;
    uint32_t addr;
    long ret;

    if ((nargs != 3) || (nret != 1)) {
        goto param_error_exit;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    addr = rtas_ld(args, 0);
    sphb = spapr_pci_find_phb(spapr, buid);
    if (!sphb || !sphb->ddw_enabled) {
        goto param_error_exit;
    }

    ret = spapr_phb_dma_reset(sphb);
    trace_spapr_iommu_ddw_reset(buid, addr, ret);
    if (ret) {
        goto hw_error_exit;
    }

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);

    return;

hw_error_exit:
    rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void spapr_rtas_ddw_init(void)
{
    spapr_rtas_register(RTAS_IBM_QUERY_PE_DMA_WINDOW,
                        "ibm,query-pe-dma-window",
                        rtas_ibm_query_pe_dma_window);
    spapr_rtas_register(RTAS_IBM_CREATE_PE_DMA_WINDOW,
                        "ibm,create-pe-dma-window",
                        rtas_ibm_create_pe_dma_window);
    spapr_rtas_register(RTAS_IBM_REMOVE_PE_DMA_WINDOW,
                        "ibm,remove-pe-dma-window",
                        rtas_ibm_remove_pe_dma_window);
    spapr_rtas_register(RTAS_IBM_RESET_PE_DMA_WINDOW,
                        "ibm,reset-pe-dma-window",
                        rtas_ibm_reset_pe_dma_window);
}

type_init(spapr_rtas_ddw_init)
