#include "vof.h"

struct prom_args {
    uint32_t service;
    uint32_t nargs;
    uint32_t nret;
    uint32_t args[10];
};

typedef unsigned long prom_arg_t;

#define ADDR(x) (uint32_t)(x)

static int prom_handle(struct prom_args *pargs)
{
    void *rtasbase;
    uint32_t rtassize = 0;
    phandle rtas;

    if (strcmp("call-method", (void *)(unsigned long)pargs->service)) {
        return -1;
    }

    if (strcmp("instantiate-rtas", (void *)(unsigned long)pargs->args[0])) {
        return -1;
    }

    rtas = ci_finddevice("/rtas");
    /* rtas-size is set by QEMU depending of FWNMI support */
    ci_getprop(rtas, "rtas-size", &rtassize, sizeof(rtassize));
    if (rtassize < hv_rtas_size) {
        printk("Error: %d bytes not enough space for RTAS, need %d\n",
               rtassize, hv_rtas_size);
        return -1;
    }

    rtasbase = (void *)(unsigned long) pargs->args[2];

    printk("*** instantiate-rtas: %x..%x\n", rtasbase, rtasbase + rtassize - 1);
    memcpy(rtasbase, hv_rtas, hv_rtas_size);
    pargs->args[pargs->nargs] = 0;
    pargs->args[pargs->nargs + 1] = pargs->args[2];

    return 0;
}

void prom_entry(uint32_t args)
{
    if (prom_handle((void *)(unsigned long) args)) {
        ci_entry(args);
    }
}

static int call_ci(const char *service, int nargs, int nret, ...)
{
    int i;
    struct prom_args args;
    va_list list;

    args.service = ADDR(service);
    args.nargs = nargs;
    args.nret = nret;

    va_start(list, nret);
    for (i = 0; i < nargs; i++) {
        args.args[i] = va_arg(list, prom_arg_t);
    }
    va_end(list);

    for (i = 0; i < nret; i++) {
        args.args[nargs + i] = 0;
    }

    if (ci_entry((uint32_t)(&args)) < 0) {
        return PROM_ERROR;
    }

    return (nret > 0) ? args.args[nargs] : 0;
}

void ci_panic(const char *str)
{
    ci_stdout(str);
    call_ci("exit", 0, 0);
}

ihandle ci_open(const char *path)
{
    return call_ci("open", 1, 1, path);
}

void ci_close(ihandle ih)
{
    call_ci("close", 1, 0, ih);
}

uint32_t ci_block_size(ihandle ih)
{
    return 512;
}

uint32_t ci_seek(ihandle ih, uint64_t offset)
{
    return call_ci("seek", 3, 1, ih, (prom_arg_t)(offset >> 32),
                   (prom_arg_t)(offset & 0xFFFFFFFFUL));
}

uint32_t ci_read(ihandle ih, void *buf, int len)
{
    return call_ci("read", 3, 1, ih, buf, len);
}

uint32_t ci_write(ihandle ih, const void *buf, int len)
{
    return call_ci("write", 3, 1, ih, buf, len);
}

phandle ci_finddevice(const char *path)
{
    return call_ci("finddevice", 1, 1, path);
}

uint32_t ci_getprop(phandle ph, const char *propname, void *prop, int len)
{
    return call_ci("getprop", 4, 1, ph, propname, prop, len);
}

void ci_stdoutn(const char *buf, int len)
{
    static ihandle istdout;

    if (!istdout) {
        phandle chosen = ci_finddevice("/chosen");

        ci_getprop(chosen, "stdout", &istdout, sizeof(istdout));
    }
    ci_write(istdout, buf, len);
}

void ci_stdout(const char *buf)
{
    ci_stdoutn(buf, strlen(buf));
}

void *ci_claim(void *virt, uint32_t size, uint32_t align)
{
    uint32_t ret = call_ci("claim", 3, 1, ADDR(virt), size, align);

    return (void *) (unsigned long) ret;
}

uint32_t ci_release(void *virt, uint32_t size)
{
    return call_ci("release", 2, 1, ADDR(virt), size);
}
