/*
 * SPAPR machine hooks to Virtual Open Firmware,
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_vio.h"
#include "hw/ppc/fdt.h"
#include "sysemu/sysemu.h"
#include "qom/qom-qobject.h"
#include "trace.h"

/* Copied from SLOF, and 4K is definitely not enough for GRUB */
#define OF_STACK_SIZE       0x8000

/* Defined as Big Endian */
struct prom_args {
    uint32_t service;
    uint32_t nargs;
    uint32_t nret;
    uint32_t args[10];
} QEMU_PACKED;

target_ulong spapr_h_vof_client(PowerPCCPU *cpu, SpaprMachineState *spapr,
                                target_ulong opcode, target_ulong *_args)
{
    target_ulong args_real = ppc64_phys_to_real(_args[0]);
    struct prom_args args_be;
    uint32_t args[ARRAY_SIZE(args_be.args)];
    uint32_t rets[ARRAY_SIZE(args_be.args)] = { 0 }, ret;
    char service[64];
    unsigned nargs, nret, i;

    if (address_space_rw(&address_space_memory, args_real,
                         MEMTXATTRS_UNSPECIFIED, &args_be, sizeof(args_be),
                         false) != MEMTX_OK) {
        return H_PARAMETER;
    }
    nargs = be32_to_cpu(args_be.nargs);
    if (nargs >= ARRAY_SIZE(args_be.args)) {
        return H_PARAMETER;
    }

    if (address_space_rw(&address_space_memory, be32_to_cpu(args_be.service),
                         MEMTXATTRS_UNSPECIFIED, service, sizeof(service),
                         false) != MEMTX_OK) {
        return H_PARAMETER;
    }
    if (strnlen(service, sizeof(service)) == sizeof(service)) {
        /* Too long service name */
        return H_PARAMETER;
    }

    for (i = 0; i < nargs; ++i) {
        args[i] = be32_to_cpu(args_be.args[i]);
    }

    nret = be32_to_cpu(args_be.nret);
    ret = vof_client_call(spapr->fdt_blob, spapr->vof, service,
                          args, nargs, rets, nret);
    if (!nret) {
        return H_SUCCESS;
    }

    args_be.args[nargs] = cpu_to_be32(ret);
    for (i = 1; i < nret; ++i) {
        args_be.args[nargs + i] = cpu_to_be32(rets[i - 1]);
    }

    if (address_space_rw(&address_space_memory,
                         args_real + offsetof(struct prom_args, args[nargs]),
                         MEMTXATTRS_UNSPECIFIED, args_be.args + nargs,
                         sizeof(args_be.args[0]) * nret, true) != MEMTX_OK) {
        return H_PARAMETER;
    }

    return H_SUCCESS;
}

void spapr_vof_client_dt_finalize(SpaprMachineState *spapr, void *fdt)
{
    char *stdout_path = spapr_vio_stdout_path(spapr->vio_bus);
    int chosen;
    size_t cb = 0;
    char *bootlist = get_boot_devices_list(&cb);

    vof_build_dt(fdt, spapr->vof);

    _FDT(chosen = fdt_path_offset(fdt, "/chosen"));
    _FDT(fdt_setprop_string(fdt, chosen, "bootargs",
                            spapr->vof->bootargs ? : ""));

    /*
     * SLOF-less setup requires an open instance of stdout for early
     * kernel printk. By now all phandles are settled so we can open
     * the default serial console.
     */
    if (stdout_path) {
        _FDT(vof_client_open_store(fdt, spapr->vof, "/chosen", "stdout",
                                   stdout_path));
        _FDT(vof_client_open_store(fdt, spapr->vof, "/chosen", "stdin",
                                   stdout_path));
    }

    _FDT(chosen = fdt_path_offset(fdt, "/chosen"));
    if (bootlist) {
        _FDT(fdt_setprop_string(fdt, chosen, "bootpath", bootlist));
    }
}

void spapr_vof_reset(SpaprMachineState *spapr, void *fdt,
                     target_ulong *stack_ptr, Error **errp)
{
    Vof *vof = spapr->vof;

    vof_init(vof, spapr->rma_size, errp);

    *stack_ptr = vof_claim(vof, 0, OF_STACK_SIZE, OF_STACK_SIZE);
    if (*stack_ptr == -1) {
        error_setg(errp, "Memory allocation for stack failed");
        return;
    }
    /* Stack grows downwards plus reserve space for the minimum stack frame */
    *stack_ptr += OF_STACK_SIZE - 0x20;

    if (spapr->kernel_size &&
        vof_claim(vof, spapr->kernel_addr, spapr->kernel_size, 0) == -1) {
        error_setg(errp, "Memory for kernel is in use");
        return;
    }

    if (spapr->initrd_size &&
        vof_claim(vof, spapr->initrd_base, spapr->initrd_size, 0) == -1) {
        error_setg(errp, "Memory for initramdisk is in use");
        return;
    }

    spapr_vof_client_dt_finalize(spapr, fdt);

    /*
     * At this point the expected allocation map is:
     *
     * 0..c38 - the initial firmware
     * 8000..10000 - stack
     * 400000.. - kernel
     * 3ea0000.. - initramdisk
     *
     * We skip writing FDT as nothing expects it; OF client interface is
     * going to be used for reading the device tree.
     */
}

void spapr_vof_quiesce(void)
{
    SpaprMachineState *spapr = SPAPR_MACHINE(qdev_get_machine());

    spapr->fdt_size = fdt_totalsize(spapr->fdt_blob);
    spapr->fdt_initial_size = spapr->fdt_size;
}

bool spapr_vof_setprop(const char *path, const char *propname, void *val,
                       int vallen)
{
    SpaprMachineState *spapr = SPAPR_MACHINE(qdev_get_machine());

    /*
     * We only allow changing properties which we know how to update in QEMU
     * OR
     * the ones which we know that they need to survive during "quiesce".
     */

    if (strcmp(path, "/rtas") == 0) {
        if (strcmp(propname, "linux,rtas-base") == 0 ||
            strcmp(propname, "linux,rtas-entry") == 0) {
            /* These need to survive quiesce so let them store in the FDT */
            return true;
        }
    }

    if (strcmp(path, "/chosen") == 0) {
        if (strcmp(propname, "bootargs") == 0) {
            Vof *vof = spapr->vof;

            g_free(vof->bootargs);
            vof->bootargs = g_strndup(val, vallen);
            return true;
        }
        if (strcmp(propname, "linux,initrd-start") == 0) {
            if (vallen == sizeof(uint32_t)) {
                spapr->initrd_base = ldl_be_p(val);
                return true;
            }
            if (vallen == sizeof(uint64_t)) {
                spapr->initrd_base = ldq_be_p(val);
                return true;
            }
            return false;
        }
        if (strcmp(propname, "linux,initrd-end") == 0) {
            if (vallen == sizeof(uint32_t)) {
                spapr->initrd_size = ldl_be_p(val) - spapr->initrd_base;
                return true;
            }
            if (vallen == sizeof(uint64_t)) {
                spapr->initrd_size = ldq_be_p(val) - spapr->initrd_base;
                return true;
            }
            return false;
        }
    }

    return true;
}
