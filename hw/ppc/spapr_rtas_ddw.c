/*
 * QEMU sPAPR Dynamic DMA windows support
 *
 * Copyright (c) 2014 Alexey Kardashevskiy, IBM Corporation.
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
#include "trace.h"

static uint32_t spapr_iommu_fixmask(struct ppc_one_seg_page_size *sps,
                                    uint32_t query_mask)
{
    int i, j;
    uint32_t mask = 0;
    const struct { int shift; uint32_t mask; } masks[] = {
        { 12, DDW_PGSIZE_4K },
        { 16, DDW_PGSIZE_64K },
        { 24, DDW_PGSIZE_16M },
        { 25, DDW_PGSIZE_32M },
        { 26, DDW_PGSIZE_64M },
        { 27, DDW_PGSIZE_128M },
        { 28, DDW_PGSIZE_256M },
        { 34, DDW_PGSIZE_16G },
    };

    for (i = 0; i < PPC_PAGE_SIZES_MAX_SZ; i++) {
        for (j = 0; j < ARRAY_SIZE(masks); ++j) {
            if ((sps[i].page_shift == masks[j].shift) &&
                    (query_mask & masks[j].mask)) {
                mask |= masks[j].mask;
            }
        }
    }

    return mask;
}

static void rtas_ibm_query_pe_dma_window(PowerPCCPU *cpu,
                                         sPAPREnvironment *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args,
                                         uint32_t nret, target_ulong rets)
{
    CPUPPCState *env = &cpu->env;
    sPAPRPHBState *sphb;
    sPAPRPHBClass *spc;
    uint64_t buid;
    uint32_t addr, pgmask = 0;
    uint32_t windows_available = 0, page_size_mask = 0, dma32_window_size = 0;
    long ret;

    if ((nargs != 3) || (nret != 5)) {
        goto param_error_exit;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    addr = rtas_ld(args, 0);
    sphb = spapr_pci_find_phb(spapr, buid);
    if (!sphb) {
        goto param_error_exit;
    }

    spc = SPAPR_PCI_HOST_BRIDGE_GET_CLASS(sphb);
    if (!spc->ddw_query) {
        goto hw_error_exit;
    }

    ret = spc->ddw_query(sphb, &windows_available, &page_size_mask,
                         &dma32_window_size);
    trace_spapr_iommu_ddw_query(buid, addr, windows_available,
                                page_size_mask, pgmask, ret);
    if (ret) {
        goto hw_error_exit;
    }

    /* Work out supported page masks */
    pgmask = spapr_iommu_fixmask(env->sps.sps, page_size_mask);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, windows_available);

    /*
     * This is "Largest contiguous block of TCEs allocated specifically
     * for (that is, are reserved for) this PE".
     * Return the maximum number as all RAM was in 4K pages.
     */
    rtas_st(rets, 2, ram_size >> SPAPR_TCE_PAGE_SHIFT);
    rtas_st(rets, 3, pgmask);
    rtas_st(rets, 4, pgmask); /* DMA migration mask */
    return;

hw_error_exit:
    rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void rtas_ibm_create_pe_dma_window(PowerPCCPU *cpu,
                                          sPAPREnvironment *spapr,
                                          uint32_t token, uint32_t nargs,
                                          target_ulong args,
                                          uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    sPAPRPHBClass *spc;
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
    if (!sphb) {
        goto param_error_exit;
    }

    spc = SPAPR_PCI_HOST_BRIDGE_GET_CLASS(sphb);
    if (!spc->ddw_create) {
        goto hw_error_exit;
    }

    page_shift = rtas_ld(args, 3);
    window_shift = rtas_ld(args, 4);
    /* Default 32bit window#0 is always there so +1 */
    liobn = SPAPR_PCI_LIOBN(sphb->index, sphb->windows_num);

    ret = spc->ddw_create(sphb, liobn, page_shift, window_shift, &tcet);
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

    object_unref(OBJECT(tcet));
    return;

hw_error_exit:
    rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
    return;

param_error_exit:
    rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
}

static void rtas_ibm_remove_pe_dma_window(PowerPCCPU *cpu,
                                          sPAPREnvironment *spapr,
                                          uint32_t token, uint32_t nargs,
                                          target_ulong args,
                                          uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    sPAPRPHBClass *spc;
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
    if (!sphb) {
        goto param_error_exit;
    }

    spc = SPAPR_PCI_HOST_BRIDGE_GET_CLASS(sphb);
    if (!spc->ddw_remove) {
        goto hw_error_exit;
    }

    ret = spc->ddw_remove(sphb, tcet);
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
                                         sPAPREnvironment *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args,
                                         uint32_t nret, target_ulong rets)
{
    sPAPRPHBState *sphb;
    sPAPRPHBClass *spc;
    uint64_t buid;
    uint32_t addr;
    long ret;

    if ((nargs != 3) || (nret != 1)) {
        goto param_error_exit;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    addr = rtas_ld(args, 0);
    sphb = spapr_pci_find_phb(spapr, buid);
    if (!sphb) {
        goto param_error_exit;
    }

    spc = SPAPR_PCI_HOST_BRIDGE_GET_CLASS(sphb);
    if (!spc->ddw_reset) {
        goto hw_error_exit;
    }

    ret = spc->ddw_reset(sphb);
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

#define SPAPR_RTAS_DDW_SWAP(n) rtas_st(rets, (n), bswap32(rtas_ld(rets, (n))))

static void rtas_ibm_query_pe_dma_window_wrong_endian(PowerPCCPU *cpu,
                                                      sPAPREnvironment *spapr,
                                                      uint32_t token,
                                                      uint32_t nargs,
                                                      target_ulong args,
                                                      uint32_t nret,
                                                      target_ulong rets)
{
    rtas_ibm_query_pe_dma_window(cpu, spapr, token, nargs, args, nret, rets);

    SPAPR_RTAS_DDW_SWAP(0);
    SPAPR_RTAS_DDW_SWAP(1);
    SPAPR_RTAS_DDW_SWAP(2);
    SPAPR_RTAS_DDW_SWAP(3);
    SPAPR_RTAS_DDW_SWAP(4);
}

static void rtas_ibm_create_pe_dma_window_wrong_endian(PowerPCCPU *cpu,
                                                       sPAPREnvironment *spapr,
                                                       uint32_t token,
                                                       uint32_t nargs,
                                                       target_ulong args,
                                                       uint32_t nret,
                                                       target_ulong rets)
{
    rtas_ibm_create_pe_dma_window(cpu, spapr, token, nargs, args, nret, rets);

    SPAPR_RTAS_DDW_SWAP(0);
    SPAPR_RTAS_DDW_SWAP(1);
    SPAPR_RTAS_DDW_SWAP(2);
    SPAPR_RTAS_DDW_SWAP(3);
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

    spapr_rtas_register_wrong_endian(RTAS_IBM_QUERY_PE_DMA_WINDOW,
                                     rtas_ibm_query_pe_dma_window_wrong_endian);
    spapr_rtas_register_wrong_endian(RTAS_IBM_CREATE_PE_DMA_WINDOW,
                                     rtas_ibm_create_pe_dma_window_wrong_endian);
}

type_init(spapr_rtas_ddw_init)
