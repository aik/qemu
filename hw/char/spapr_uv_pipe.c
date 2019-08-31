#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/qdev.h"
#include "chardev/char-fe.h"
#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_vio.h"
#include "sysemu/hw_accel.h"

#define VTERM_BUFSIZE   16

typedef struct SpaprVioUv {
    SpaprVioDevice sdev;
    CharBackend chardev;
    uint32_t in, out;
    uint8_t buf[VTERM_BUFSIZE];
} SpaprVioUv;

#define TYPE_VIO_SPAPR_UV_DEVICE "spapr-uv"
#define VIO_SPAPR_UV_DEVICE(obj) \
     OBJECT_CHECK(SpaprVioUv, (obj), TYPE_VIO_SPAPR_UV_DEVICE)

static int vty_can_receive(void *opaque)
{
    SpaprVioUv *dev = VIO_SPAPR_UV_DEVICE(opaque);

    return VTERM_BUFSIZE - (dev->in - dev->out);
}

static void spapr_do_excp(CPUState *cs, run_on_cpu_data arg)
{
    cpu_synchronize_state(cs);
    cs->exception_index = POWERPC_EXCP_ALIGN;
    ppc_cpu_do_interrupt(cs);
}

static void vty_receive(void *opaque, const uint8_t *buf, int size)
{
    SpaprMachineState *spapr = SPAPR_MACHINE(qdev_get_machine());
    char b[256];
    CPUState *cs;

    memcpy(b, buf, size);
    b[size] = 0;
    printf("+++Q+++ (%u) %s %u: %s\n", getpid(), __func__, __LINE__, b);
    if (spapr->guest_buf_addr) {
        cpu_physical_memory_write(spapr->guest_buf_addr, b, strlen(b) + 1);
    } else {
        printf("+++Q+++ (%u) %s %u: Skipping first write\n", getpid(), __func__, __LINE__);
    }

    CPU_FOREACH(cs) {
        if (cs->cpu_index != 0) {
            continue;
        }
        async_run_on_cpu(cs, spapr_do_excp, RUN_ON_CPU_NULL);
    }
}

static void spapr_uv_realize(SpaprVioDevice *sdev, Error **errp)
{
    SpaprMachineState *spapr = SPAPR_MACHINE(qdev_get_machine());
    SpaprVioUv *dev = VIO_SPAPR_UV_DEVICE(sdev);

    if (!qemu_chr_fe_backend_connected(&dev->chardev)) {
        error_setg(errp, "chardev property not set");
        return;
    }

    qemu_chr_fe_set_handlers(&dev->chardev, vty_can_receive,
                             vty_receive, NULL, NULL, dev, NULL, true);

    spapr->uvdev = &dev->chardev;
}

/* Forward declaration */
static target_ulong h_uv_pipe(PowerPCCPU *cpu, SpaprMachineState *spapr,
                                    target_ulong opcode, target_ulong *args)
{
    target_ulong ptr = args[0];
    char buf[256];

    cpu_physical_memory_read(ptr, buf, sizeof(buf));
    printf("+++Q+++ (%u) %s %u: %s %lx\n", getpid(), __func__, __LINE__, buf, ptr);
    qemu_chr_fe_write_all(spapr->uvdev, (uint8_t *)buf, strlen(buf));
    spapr->guest_buf_addr = ptr;

    return H_SUCCESS;
}

static Property spapr_uv_properties[] = {
    DEFINE_SPAPR_PROPERTIES(SpaprVioUv, sdev),
    DEFINE_PROP_CHR("chardev", SpaprVioUv, chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_spapr_uv = {
    .name = "spapr_uv",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_SPAPR_VIO(sdev, SpaprVioUv),

        VMSTATE_UINT32(in, SpaprVioUv),
        VMSTATE_UINT32(out, SpaprVioUv),
        VMSTATE_BUFFER(buf, SpaprVioUv),
        VMSTATE_END_OF_LIST()
    },
};

static void spapr_uv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SpaprVioDeviceClass *k = VIO_SPAPR_DEVICE_CLASS(klass);

    k->realize = spapr_uv_realize;
    k->dt_name = "vty";
    k->dt_type = "serial";
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->props = spapr_uv_properties;
    dc->vmsd = &vmstate_spapr_uv;
}

static const TypeInfo spapr_uv_info = {
    .name          = TYPE_VIO_SPAPR_UV_DEVICE,
    .parent        = TYPE_VIO_SPAPR_DEVICE,
    .instance_size = sizeof(SpaprVioUv),
    .class_init    = spapr_uv_class_init,
};

static void spapr_uv_register_types(void)
{
    spapr_register_hypercall(0xf004, h_uv_pipe);
    type_register_static(&spapr_uv_info);
}

type_init(spapr_uv_register_types)
