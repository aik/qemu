#include "vof.h"

void boot_from_memory(uint64_t initrd, uint64_t initrdsize)
{
    uint64_t kern[2];
    phandle chosen = ci_finddevice("/chosen");

    if (ci_getprop(chosen, "qemu,boot-kernel", kern, sizeof(kern)) !=
        sizeof(kern)) {
        return;
    }

    ci_prn("printf1", (uint32_t) kern[0], (uint32_t) kern[1]);
    do_boot(kern[0], initrd, initrdsize);
    //ci_prn("printf2", 0, 0);
}
