/*
 * QEMU PowerPC Virtual Open Firmware.
 *
 * This implements client interface from OpenFirmware IEEE1275 on the QEMU
 * side to leave only a very basic firmware in the VM.
 *
 * Copyright (c) 2021 IBM Corporation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/timer.h"
#include "qemu/range.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include <sys/ioctl.h>
#include "exec/ram_addr.h"
#include "exec/address-spaces.h"
#include "hw/ppc/vof.h"
#include "hw/ppc/fdt.h"
#include "sysemu/runstate.h"
#include "qom/qom-qobject.h"
#include "trace.h"
#include "chardev/char-fe.h"
#include "hw/ppc/spapr_vio.h"
#include <libfdt.h>
#include "hw/block/block.h"
#include "sysemu/block-backend.h"

/*
 * OF 1275 "nextprop" description suggests is it 32 bytes max but
 * LoPAPR defines "ibm,query-interrupt-source-number" which is 33 chars long.
 */
#define OF_PROPNAME_LEN_MAX 64

#define VOF_MAX_PATH        256
#define VOF_MAX_SETPROPLEN  2048
#define VOF_MAX_METHODLEN   256
#define VOF_MAX_FORTHCODE   256
#define VOF_VTY_BUF_SIZE    256

typedef struct {
    uint64_t start;
    uint64_t size;
} OfClaimed;

typedef struct {
    char *path; /* the path used to open the instance */
    uint32_t phandle;
    DeviceState *dev;
    CharBackend *cbe;
    BlockBackend *blk;
    uint64_t blk_pos;
    uint16_t blk_physical_block_size;
    char *params;
} OfInstance;

#define VOF_MEM_READ(pa, buf, size) \
    address_space_read_full(&address_space_memory, \
    (pa), MEMTXATTRS_UNSPECIFIED, (buf), (size))
#define VOF_MEM_WRITE(pa, buf, size) \
    address_space_write(&address_space_memory, \
    (pa), MEMTXATTRS_UNSPECIFIED, (buf), (size))

static void dump_ih_cb(gpointer key, gpointer value, gpointer user_data)
{
    printf("+++Q+++ (%u) %s %u: %lx %lx\n", getpid(), __func__, __LINE__,
            (unsigned long) key,  (unsigned long) value);
}

static void dump_ih(Vof *vof)
{
    g_hash_table_foreach(vof->of_instances, dump_ih_cb, NULL);
}

static int readstr(hwaddr pa, char *buf, int size)
{
    if (VOF_MEM_READ(pa, buf, size) != MEMTX_OK) {
        return -1;
    }
    if (strnlen(buf, size) == size) {
        buf[size - 1] = '\0';
        trace_vof_error_str_truncated(buf, size);
        return -1;
    }
    return 0;
}

static bool cmpservice(const char *s, unsigned nargs, unsigned nret,
                       const char *s1, unsigned nargscheck, unsigned nretcheck)
{
    if (strcmp(s, s1)) {
        return false;
    }
    if ((nargscheck && (nargs != nargscheck)) ||
        (nretcheck && (nret != nretcheck))) {
        trace_vof_error_param(s, nargscheck, nretcheck, nargs, nret);
        return false;
    }

    return true;
}

#if 0
static void split_path(const char *fullpath, char **node, char **unit,
                       char **part)
{
    const char *c, *p = NULL, *u = NULL;

    *node = *unit = *part = NULL;

    if (fullpath[0] == '\0') {
        *node = g_strdup(fullpath);
        return;
    }

    for (c = fullpath + strlen(fullpath) - 1; c > fullpath; --c) {
        if (*c == '/') {
            break;
        }
        if (*c == ':') {
            p = c + 1;
            continue;
        }
        if (*c == '@') {
            u = c + 1;
            continue;
        }
    }

    if (p && u && p < u) {
        p = NULL;
    }

    if (u && p) {
        *node = g_strndup(fullpath, u - fullpath - 1);
        *unit = g_strndup(u, p - u - 1);
        *part = g_strdup(p);
    } else if (!u && p) {
        *node = g_strndup(fullpath, p - fullpath - 1);
        *part = g_strdup(p);
    } else if (!p && u) {
        *node = g_strndup(fullpath, u - fullpath - 1);
        *unit = g_strdup(u);
    } else {
        *node = g_strdup(fullpath);
    }
}
#endif

static void prop_format(char *tval, int tlen, const void *prop, int len)
{
    int i;
    const unsigned char *c;
    char *t;
    const char bin[] = "...";

    for (i = 0, c = prop; i < len; ++i, ++c) {
        if (*c == '\0' && i == len - 1) {
            strncpy(tval, prop, tlen - 1);
            return;
        }
        if (*c < 0x20 || *c >= 0x80) {
            break;
        }
    }

    for (i = 0, c = prop, t = tval; i < len; ++i, ++c) {
        if (t >= tval + tlen - sizeof(bin) - 1 - 2 - 1) {
            strcpy(t, bin);
            return;
        }
        if (i && i % 4 == 0 && i != len - 1) {
            strcat(t, " ");
            ++t;
        }
        t += sprintf(t, "%02X", *c & 0xFF);
    }
}

static int get_path(const void *fdt, int offset, char *buf, int len)
{
    int ret;

    ret = fdt_get_path(fdt, offset, buf, len - 1);
    if (ret < 0) {
        return ret;
    }

    buf[len - 1] = '\0';

    return strlen(buf) + 1;
}

static int phandle_to_path(const void *fdt, uint32_t ph, char *buf, int len)
{
    int ret;

    ret = fdt_node_offset_by_phandle(fdt, ph);
    if (ret < 0) {
        return ret;
    }

    return get_path(fdt, ret, buf, len);
}

static int path_offset(const void *fdt, const char *path, int pathlen)
{
    int offset, i;

    if (!pathlen) {
        return -1;
    }
    offset = fdt_path_offset_namelen(fdt, path, pathlen);
    if (offset >= 0) {
        return offset;
    }

    /*
     * fdt_nodename_eq_ handles "@" in the FDT's node name but cannot
     * match an unit-less node name with a path which includes the unit,
     * so do it here.
     */
    for (i = pathlen - 1; i > 0; --i) {
        if (path[i] == '/') {
            return -1;
        }
        if (path[i] == '@') {
            return fdt_path_offset_namelen(fdt, path, i);
        }
    }
    return -1;
}

static uint32_t vof_finddevice(const void *fdt, uint32_t nodeaddr)
{
    char fullnode[VOF_MAX_PATH];
    uint32_t ret = -1;
    int offset;

    if (readstr(nodeaddr, fullnode, sizeof(fullnode))) {
        return (uint32_t) ret;
    }

    offset = path_offset(fdt, fullnode, strlen(fullnode));
    if (offset >= 0) {
        ret = fdt_get_phandle(fdt, offset);
    }
    trace_vof_finddevice(fullnode, ret);
    return (uint32_t) ret;
}

static const void *getprop(const void *fdt, int nodeoff, const char *propname,
                           int *proplen, bool *write0)
{
    const char *unit, *prop;

    /*
     * The "name" property is not actually stored as a property in the FDT,
     * we emulate it by returning a pointer to the node's name and adjust
     * proplen to include only the name but not the unit.
     */
    if (strcmp(propname, "name") == 0) {
        prop = fdt_get_name(fdt, nodeoff, proplen);
        if (!prop) {
            *proplen = 0;
            return NULL;
        }

        unit = memchr(prop, '@', *proplen);
        if (unit) {
            *proplen = unit - prop;
        }
        *proplen += 1;

        /*
         * Since it might be cut at "@" and there will be no trailing zero
         * in the prop buffer, tell the caller to write zero at the end.
         */
        if (write0) {
            *write0 = true;
        }
        return prop;
    }

    if (write0) {
        *write0 = false;
    }
    return fdt_getprop(fdt, nodeoff, propname, proplen);
}

static uint32_t vof_getprop(const void *fdt, uint32_t nodeph, uint32_t pname,
                            uint32_t valaddr, uint32_t vallen)
{
    char propname[OF_PROPNAME_LEN_MAX + 1];
    uint32_t ret = 0;
    int proplen = 0;
    const void *prop;
    char trval[64] = "";
    int nodeoff = fdt_node_offset_by_phandle(fdt, nodeph);
    bool write0;

    if (nodeoff < 0) {
        return -1;
    }
    if (readstr(pname, propname, sizeof(propname))) {
        return -1;
    }
    prop = getprop(fdt, nodeoff, propname, &proplen, &write0);
    if (prop) {
        const char zero = 0;
        int cb = MIN(proplen, vallen);

        if (VOF_MEM_WRITE(valaddr, prop, cb) != MEMTX_OK ||
            /* if that was "name" with a unit address, overwrite '@' with '0' */
            (write0 &&
             cb == proplen &&
             VOF_MEM_WRITE(valaddr + cb - 1, &zero, 1) != MEMTX_OK)) {
            ret = -1;
        } else {
            /*
             * OF1275 says:
             * "Size is either the actual size of the property, or -1 if name
             * does not exist", hence returning proplen instead of cb.
             */
            ret = proplen;
            /* Do not format a value if tracepoint is silent, for performance */
            if (trace_event_get_state(TRACE_VOF_GETPROP) &&
                qemu_loglevel_mask(LOG_TRACE)) {
                prop_format(trval, sizeof(trval), prop, ret);
            }
        }
    } else {
        ret = -1;
    }
    trace_vof_getprop(nodeph, propname, ret, trval);

    return ret;
}

static uint32_t vof_getproplen(const void *fdt, uint32_t nodeph, uint32_t pname)
{
    char propname[OF_PROPNAME_LEN_MAX + 1];
    uint32_t ret = 0;
    int proplen = 0;
    const void *prop;
    int nodeoff = fdt_node_offset_by_phandle(fdt, nodeph);

    if (nodeoff < 0) {
        return -1;
    }
    if (readstr(pname, propname, sizeof(propname))) {
        return -1;
    }
    prop = getprop(fdt, nodeoff, propname, &proplen, NULL);
    if (prop) {
        ret = proplen;
    } else {
        ret = -1;
    }
    trace_vof_getproplen(nodeph, propname, ret);

    return ret;
}

static uint32_t vof_setprop(void *fdt, Vof *vof,
                            uint32_t nodeph, uint32_t pname,
                            uint32_t valaddr, uint32_t vallen)
{
    char propname[OF_PROPNAME_LEN_MAX + 1];
    uint32_t ret = -1;
    int offset;
    char trval[64] = "";
    char nodepath[VOF_MAX_PATH] = "";
    Object *vmo = object_dynamic_cast(qdev_get_machine(), TYPE_VOF_MACHINE_IF);
    g_autofree char *val = NULL;

    if (vallen > VOF_MAX_SETPROPLEN) {
        goto trace_exit;
    }
    if (readstr(pname, propname, sizeof(propname))) {
        goto trace_exit;
    }
    offset = fdt_node_offset_by_phandle(fdt, nodeph);
    if (offset < 0) {
        goto trace_exit;
    }
    ret = get_path(fdt, offset, nodepath, sizeof(nodepath));
    if (ret <= 0) {
        goto trace_exit;
    }

    val = g_malloc0(vallen);
    if (VOF_MEM_READ(valaddr, val, vallen) != MEMTX_OK) {
        goto trace_exit;
    }

    if (vmo) {
        VofMachineIfClass *vmc = VOF_MACHINE_GET_CLASS(vmo);

        if (!vmc->setprop(nodepath, propname, val, vallen)) {
            goto trace_exit;
        }
    }

    ret = fdt_setprop(fdt, offset, propname, val, vallen);
    if (ret) {
        goto trace_exit;
    }

    if (trace_event_get_state(TRACE_VOF_SETPROP) &&
        qemu_loglevel_mask(LOG_TRACE)) {
        prop_format(trval, sizeof(trval), val, vallen);
    }
    ret = vallen;

trace_exit:
    trace_vof_setprop(nodeph, propname, trval, vallen, ret);

    return ret;
}

static uint32_t vof_nextprop(const void *fdt, uint32_t phandle,
                             uint32_t prevaddr, uint32_t nameaddr)
{
    int offset, nodeoff = fdt_node_offset_by_phandle(fdt, phandle);
    char prev[OF_PROPNAME_LEN_MAX + 1];
    const char *tmp;

    if (readstr(prevaddr, prev, sizeof(prev))) {
        return -1;
    }

    fdt_for_each_property_offset(offset, fdt, nodeoff) {
        if (!fdt_getprop_by_offset(fdt, offset, &tmp, NULL)) {
            return 0;
        }
        if (prev[0] == '\0' || strcmp(prev, tmp) == 0) {
            if (prev[0] != '\0') {
                offset = fdt_next_property_offset(fdt, offset);
                if (offset < 0) {
                    return 0;
                }
            }
            if (!fdt_getprop_by_offset(fdt, offset, &tmp, NULL)) {
                return 0;
            }

            if (VOF_MEM_WRITE(nameaddr, tmp, strlen(tmp) + 1) != MEMTX_OK) {
                return -1;
            }
            return 1;
        }
    }

    return 0;
}

static uint32_t vof_peer(const void *fdt, uint32_t phandle)
{
    int ret;

    if (phandle == 0) {
        ret = fdt_path_offset(fdt, "/");
    } else {
        ret = fdt_next_subnode(fdt, fdt_node_offset_by_phandle(fdt, phandle));
    }

    if (ret < 0) {
        ret = 0;
    } else {
        ret = fdt_get_phandle(fdt, ret);
    }

    return ret;
}

static uint32_t vof_child(const void *fdt, uint32_t phandle)
{
    int ret = fdt_first_subnode(fdt, fdt_node_offset_by_phandle(fdt, phandle));

    if (ret < 0) {
        ret = 0;
    } else {
        ret = fdt_get_phandle(fdt, ret);
    }

    return ret;
}

static uint32_t vof_parent(const void *fdt, uint32_t phandle)
{
    int ret = fdt_parent_offset(fdt, fdt_node_offset_by_phandle(fdt, phandle));

    if (ret < 0) {
        ret = 0;
    } else {
        ret = fdt_get_phandle(fdt, ret);
    }

    return ret;
}

static DeviceState *of_client_find_qom_dev(BusState *bus, const char *path,
                                           int pathlen)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        const char *p = qdev_get_fw_dev_path(kid->child);
        BusState *child;

        if (p && strncmp(path, p, pathlen) == 0) {
            return kid->child;
        }
        QLIST_FOREACH(child, &kid->child->child_bus, sibling) {
            DeviceState *d = of_client_find_qom_dev(child, path, pathlen);

            if (d) {
                return d;
            }
        }
    }
    return NULL;
}

static uint32_t vof_do_open(void *fdt, Vof *vof, const char *path)
{
    int offset, pathlen = strlen(path);
    char *params;
    uint32_t ret = 0;
    OfInstance *inst = NULL;
    g_autofree char *node = NULL, *unit = NULL, *part = NULL;

    if (vof->of_instance_last == 0xFFFFFFFF) {
        /* We do not recycle ihandles yet */
        goto trace_exit;
    }

    params = memrchr(path, ':', pathlen);
    if (params) {
        pathlen = params - path;
    }

    offset = path_offset(fdt, path, pathlen);
    if (offset < 0) {
        trace_vof_error_unknown_path(path);
        goto trace_exit;
    }

    inst = g_new0(OfInstance, 1);
    inst->phandle = fdt_get_phandle(fdt, offset);
    g_assert(inst->phandle);
    ++vof->of_instance_last;

    inst->dev = of_client_find_qom_dev(sysbus_get_default(), path, pathlen);
    inst->path = g_strdup(path);
    inst->params = g_strdup(part);
    g_hash_table_insert(vof->of_instances,
                        GINT_TO_POINTER(vof->of_instance_last),
                        inst);
    ret = vof->of_instance_last;

    if (inst->dev) {
        const char *cdevstr = object_property_get_str(OBJECT(inst->dev),
                                                      "chardev", NULL);
        const char *blkstr = object_property_get_str(OBJECT(inst->dev),
                                                     "drive", NULL);

        if (cdevstr) {
            Chardev *cdev = qemu_chr_find(cdevstr);

            if (cdev) {
                inst->cbe = cdev->be;
            }
        } else if (blkstr) {
            BlockConf conf = { 0 };

            if (params && params[0] && strcmp(params, "0")) {
                error_report("Warning: only :0 is supported for disks");
            }

            inst->blk = blk_by_name(blkstr);
            conf.blk = inst->blk;
            blkconf_blocksizes(&conf, NULL);
            inst->blk_physical_block_size = conf.physical_block_size;
        }
    }

trace_exit:
    trace_vof_open(path, inst ? inst->phandle : 0, ret);

    return ret;
}

uint32_t vof_client_open_store(void *fdt, Vof *vof, const char *nodename,
                               const char *prop, const char *path)
{
    int node = fdt_path_offset(fdt, nodename);
    uint32_t inst = vof_do_open(fdt, vof, path);

    return fdt_setprop_cell(fdt, node, prop, inst);
}

static uint32_t vof_open(void *fdt, Vof *vof, uint32_t pathaddr)
{
    char path[VOF_MAX_PATH];
    uint32_t ret;

    if (readstr(pathaddr, path, sizeof(path))) {
        return -1;
    }

    ret = vof_do_open(fdt, vof, path);
    dump_ih(vof);
    return ret;
}

static void vof_close(Vof *vof, uint32_t ihandle)
{
    dump_ih(vof);
    if (!g_hash_table_remove(vof->of_instances, GINT_TO_POINTER(ihandle))) {
        trace_vof_error_unknown_ihandle_close(ihandle);
    }
}

static uint32_t vof_instance_to_package(Vof *vof, uint32_t ihandle)
{
    gpointer instp = g_hash_table_lookup(vof->of_instances,
                                         GINT_TO_POINTER(ihandle));
    uint32_t ret = -1;

    if (instp) {
        ret = ((OfInstance *)instp)->phandle;
    }
    trace_vof_instance_to_package(ihandle, ret);

    return ret;
}

static uint32_t vof_package_to_path(const void *fdt, uint32_t phandle,
                                    uint32_t buf, uint32_t len)
{
    uint32_t ret = -1;
    char tmp[VOF_MAX_PATH] = "";

    ret = phandle_to_path(fdt, phandle, tmp, sizeof(tmp));
    if (ret > 0) {
        if (VOF_MEM_WRITE(buf, tmp, ret) != MEMTX_OK) {
            ret = -1;
        }
    }

    trace_vof_package_to_path(phandle, tmp, ret);

    return ret;
}

static uint32_t vof_instance_to_path(void *fdt, Vof *vof, uint32_t ihandle,
                                     uint32_t buf, uint32_t len)
{
    uint32_t ret = -1;
    uint32_t phandle = vof_instance_to_package(vof, ihandle);
    char tmp[VOF_MAX_PATH] = "";

    if (phandle != -1) {
        ret = phandle_to_path(fdt, phandle, tmp, sizeof(tmp));
        if (ret > 0) {
            if (VOF_MEM_WRITE(buf, tmp, ret) != MEMTX_OK) {
                ret = -1;
            }
        }
    }
    trace_vof_instance_to_path(ihandle, phandle, tmp, ret);

    return ret;
}

static uint32_t vof_write(Vof *vof, uint32_t ihandle, uint32_t buf,
                          uint32_t len)
{
    char tmp[VOF_VTY_BUF_SIZE];
    unsigned cb;
    OfInstance *inst = (OfInstance *)
        g_hash_table_lookup(vof->of_instances, GINT_TO_POINTER(ihandle));

    if (!inst) {
        trace_vof_error_write(ihandle);
        return -1;
    }

    for ( ; len > 0; len -= cb) {
        cb = MIN(len, sizeof(tmp) - 1);
        if (VOF_MEM_READ(buf, tmp, cb) != MEMTX_OK) {
            return -1;
        }

        if (inst->cbe) {
            qemu_chr_fe_write_all(inst->cbe, (uint8_t *) tmp, cb);
        }
        if (inst->blk) {
            /* Do not allow writing to the boot disk, just a precation */
            len = -1;
            trace_vof_blk_write(ihandle, len);
            break;
        } else if (trace_event_get_state(TRACE_VOF_WRITE) &&
            qemu_loglevel_mask(LOG_TRACE)) {
            tmp[cb] = '\0';
            trace_vof_write(ihandle, cb, tmp);
        }
    }

    return len;
}

static uint32_t vof_read(Vof *vof, uint32_t ihandle, uint32_t bufaddr,
                         uint32_t len)
{
    uint32_t ret = 0;
    OfInstance *inst = (OfInstance *)
        g_hash_table_lookup(vof->of_instances, GINT_TO_POINTER(ihandle));

    if (inst) {
        hwaddr xlat = 0;
        hwaddr xlen = len;
        MemoryRegion *mr = address_space_translate(&address_space_memory,
                                                   bufaddr, &xlat, &xlen, true,
                                                   MEMTXATTRS_UNSPECIFIED);

        if (mr && xlen == len) {
            uint8_t *buf = memory_region_get_ram_ptr(mr) + xlat;

            if (inst->cbe) {
                SpaprVioDevice *sdev;

                sdev = (SpaprVioDevice *) object_dynamic_cast(
                        OBJECT(inst->dev), TYPE_VIO_SPAPR_DEVICE);
                if (sdev) {
                    ret = vty_getchars(sdev, buf, len);
                }
            } else if (inst->blk) {
                int rc = blk_pread(inst->blk, inst->blk_pos, buf, len);

                if (rc > 0) {
                    ret = rc;
                }
                trace_vof_blk_read(ihandle, inst->blk_pos, len, ret);
                if (rc > 0) {
                    inst->blk_pos += rc;
                }
            }
        }
    }

    return ret;
}

static uint32_t vof_seek(Vof *vof, uint32_t ihandle, uint32_t hi, uint32_t lo)
{
    uint32_t ret = -1;
    uint64_t pos = ((uint64_t) hi << 32) | lo;
    OfInstance *inst = (OfInstance *)
        g_hash_table_lookup(vof->of_instances, GINT_TO_POINTER(ihandle));

    if (inst) {
        if (inst->blk) {
            inst->blk_pos = pos;
            ret = 1;
            trace_vof_blk_seek(ihandle, pos, ret);
        }
    }

    return ret;
}

static void vof_claimed_dump(GArray *claimed)
{
    int i;
    OfClaimed c;

    if (trace_event_get_state(TRACE_VOF_CLAIMED) &&
        qemu_loglevel_mask(LOG_TRACE)) {

        for (i = 0; i < claimed->len; ++i) {
            c = g_array_index(claimed, OfClaimed, i);
            trace_vof_claimed(c.start, c.start + c.size, c.size);
        }
    }
}

static bool vof_claim_avail(GArray *claimed, uint64_t virt, uint64_t size)
{
    int i;
    OfClaimed c;

    for (i = 0; i < claimed->len; ++i) {
        c = g_array_index(claimed, OfClaimed, i);
        if (ranges_overlap(c.start, c.size, virt, size)) {
            return false;
        }
    }

    return true;
}

static void vof_claim_add(GArray *claimed, uint64_t virt, uint64_t size)
{
    OfClaimed newclaim;

    newclaim.start = virt;
    newclaim.size = size;
    g_array_append_val(claimed, newclaim);
}

static gint of_claimed_compare_func(gconstpointer a, gconstpointer b)
{
    return ((OfClaimed *)a)->start - ((OfClaimed *)b)->start;
}

static void vof_dt_memory_available(void *fdt, GArray *claimed, uint64_t base)
{
    int i, n, offset, proplen = 0;
    uint64_t *mem0_reg;
    g_autofree struct { uint64_t start, size; } *avail = NULL;

    if (!fdt || !claimed) {
        return;
    }

    offset = fdt_path_offset(fdt, "/memory@0");
    _FDT(offset);

    mem0_reg = (uint64_t *) fdt_getprop(fdt, offset, "reg", &proplen);
    g_assert(mem0_reg && proplen == 2 * sizeof(uint64_t));

    g_array_sort(claimed, of_claimed_compare_func);
    vof_claimed_dump(claimed);

    /*
     * VOF resides in the first page so we do not need to check if there is
     * available memory before the first claimed block
     */
    g_assert(claimed->len && (g_array_index(claimed, OfClaimed, 0).start == 0));

    avail = g_malloc0(sizeof(avail[0]) * claimed->len);
    for (i = 0, n = 0; i < claimed->len; ++i) {
        OfClaimed c = g_array_index(claimed, OfClaimed, i);
        uint64_t start, size;

        start = c.start + c.size;
        if (i < claimed->len - 1) {
            OfClaimed cn = g_array_index(claimed, OfClaimed, i + 1);

            size = cn.start - start;
        } else {
            size = be64_to_cpu(mem0_reg[1]) - start;
        }
        avail[n].start = cpu_to_be64(start);
        avail[n].size = cpu_to_be64(size);

        if (size) {
            trace_vof_avail(c.start + c.size, c.start + c.size + size, size);
            ++n;
        }
    }
    _FDT((fdt_setprop(fdt, offset, "available", avail, sizeof(avail[0]) * n)));
}

/*
 * OF1275:
 * "Allocates size bytes of memory. If align is zero, the allocated range
 * begins at the virtual address virt. Otherwise, an aligned address is
 * automatically chosen and the input argument virt is ignored".
 *
 * In other words, exactly one of @virt and @align is non-zero.
 */
uint64_t vof_claim(Vof *vof, uint64_t virt, uint64_t size,
                   uint64_t align)
{
    uint64_t ret;

    if (size == 0) {
        ret = -1;
    } else if (align == 0) {
        if (!vof_claim_avail(vof->claimed, virt, size)) {
            ret = -1;
        } else {
            ret = virt;
        }
    } else {
        vof->claimed_base = QEMU_ALIGN_UP(vof->claimed_base, align);
        while (1) {
            if (vof->claimed_base >= vof->top_addr) {
                error_report("Out of RMA memory for the OF client");
                return -1;
            }
            if (vof_claim_avail(vof->claimed, vof->claimed_base, size)) {
                break;
            }
            vof->claimed_base += size;
        }
        ret = vof->claimed_base;
    }

    if (ret != -1) {
        vof->claimed_base = MAX(vof->claimed_base, ret + size);
        vof_claim_add(vof->claimed, ret, size);
    }
    trace_vof_claim(virt, size, align, ret);

    return ret;
}

static uint32_t vof_release(Vof *vof, uint64_t virt, uint64_t size)
{
    uint32_t ret = -1;
    int i;
    GArray *claimed = vof->claimed;
    OfClaimed c;

    for (i = 0; i < claimed->len; ++i) {
        c = g_array_index(claimed, OfClaimed, i);
        if (c.start == virt && c.size == size) {
            g_array_remove_index(claimed, i);
            ret = 0;
            break;
        }
    }

    trace_vof_release(virt, size, ret);

    return ret;
}

static void vof_instantiate_rtas(Error **errp)
{
    error_setg(errp, "The firmware should have instantiated RTAS");
}

static uint32_t vof_call_method(Vof *vof, uint32_t methodaddr, uint32_t ihandle,
                                uint32_t param1, uint32_t param2,
                                uint32_t param3, uint32_t param4,
                                uint32_t *ret2)
{
    uint32_t ret = -1;
    char method[VOF_MAX_METHODLEN] = "";
    OfInstance *inst;

    if (!ihandle) {
        goto trace_exit;
    }

    inst = (OfInstance *) g_hash_table_lookup(vof->of_instances,
                                              GINT_TO_POINTER(ihandle));
    if (!inst) {
        goto trace_exit;
    }

    if (readstr(methodaddr, method, sizeof(method))) {
        goto trace_exit;
    }

    if (strcmp(inst->path, "/") == 0) {
        if (strcmp(method, "ibm,client-architecture-support") == 0) {
            Object *vmo = object_dynamic_cast(qdev_get_machine(),
                                              TYPE_VOF_MACHINE_IF);

            if (vmo) {
                VofMachineIfClass *vmc = VOF_MACHINE_GET_CLASS(vmo);

                ret = vmc->client_architecture_support(first_cpu, param1);
            }

            *ret2 = 0;
        }
    } else if (strcmp(inst->path, "/rtas") == 0) {
        if (strcmp(method, "instantiate-rtas") == 0) {
            vof_instantiate_rtas(&error_fatal);
            ret = 0;
            *ret2 = param1; /* rtas-base */
        }
    } else if (inst->blk) {
        if (strcmp(method, "block-size") == 0) {
            ret = 0;
            *ret2 = inst->blk_physical_block_size;
        } else if (strcmp(method, "#blocks") == 0) {
            ret = 0;
            *ret2 = blk_getlength(inst->blk) / inst->blk_physical_block_size;
        }
     } else if (inst->dev) {
        if (strcmp(method, "vscsi-report-luns") == 0) {
            /* TODO: Not implemented yet, not clear when it is really needed */
            ret = -1;
            *ret2 = 1;
        }

    } else {
        trace_vof_error_unknown_method(method);
    }

trace_exit:
    trace_vof_method(ihandle, method, param1, ret, *ret2);

    return ret;
}

static uint32_t vof_call_interpret(uint32_t cmdaddr, uint32_t param1,
                                   uint32_t param2, uint32_t *ret2)
{
    uint32_t ret = -1;
    char cmd[VOF_MAX_FORTHCODE] = "";

    /* No interpret implemented, just trace it */
    readstr(cmdaddr, cmd, sizeof(cmd));
    trace_vof_interpret(cmd, param1, param2, ret, *ret2);

    return ret;
}

static void vof_quiesce(void *fdt, Vof *vof)
{
    Object *vmo = object_dynamic_cast(qdev_get_machine(), TYPE_VOF_MACHINE_IF);
    /* After "quiesce", no change is expected to the FDT, pack FDT to ensure */
    int rc = fdt_pack(fdt);

    assert(rc == 0);

    if (vmo) {
        VofMachineIfClass *vmc = VOF_MACHINE_GET_CLASS(vmo);

        vmc->quiesce();
    }

    vof_claimed_dump(vof->claimed);
    vof->quiesced = true;
}

uint32_t vof_client_call(void *fdt, Vof *vof, const char *service,
                         uint32_t *args, unsigned nargs,
                         uint32_t *rets, unsigned nrets)
{
    uint32_t ret = 0;

    /* @nrets includes the value which this function returns */
#define cmpserv(s, a, r) \
    cmpservice(service, nargs, nrets, (s), (a), (r))

    /* This is not a bug if CI is called after "quiesce" but still suspicios */
    if (vof->quiesced) {
        trace_vof_warn_quiesced();
    }

    if (cmpserv("finddevice", 1, 1)) {
        ret = vof_finddevice(fdt, args[0]);
    } else if (cmpserv("getprop", 4, 1)) {
        ret = vof_getprop(fdt, args[0], args[1], args[2], args[3]);
    } else if (cmpserv("getproplen", 2, 1)) {
        ret = vof_getproplen(fdt, args[0], args[1]);
    } else if (cmpserv("setprop", 4, 1)) {
        ret = vof_setprop(fdt, vof, args[0], args[1], args[2], args[3]);
    } else if (cmpserv("nextprop", 3, 1)) {
        ret = vof_nextprop(fdt, args[0], args[1], args[2]);
    } else if (cmpserv("peer", 1, 1)) {
        ret = vof_peer(fdt, args[0]);
    } else if (cmpserv("child", 1, 1)) {
        ret = vof_child(fdt, args[0]);
    } else if (cmpserv("parent", 1, 1)) {
        ret = vof_parent(fdt, args[0]);
    } else if (cmpserv("open", 1, 1)) {
        ret = vof_open(fdt, vof, args[0]);
    } else if (cmpserv("close", 1, 0)) {
        vof_close(vof, args[0]);
    } else if (cmpserv("instance-to-package", 1, 1)) {
        ret = vof_instance_to_package(vof, args[0]);
    } else if (cmpserv("package-to-path", 3, 1)) {
        ret = vof_package_to_path(fdt, args[0], args[1], args[2]);
    } else if (cmpserv("instance-to-path", 3, 1)) {
        ret = vof_instance_to_path(fdt, vof, args[0], args[1], args[2]);
    } else if (cmpserv("write", 3, 1)) {
        ret = vof_write(vof, args[0], args[1], args[2]);
    } else if (cmpserv("read", 3, 1)) {
        ret = vof_read(vof, args[0], args[1], args[2]);
    } else if (cmpserv("seek", 3, 1)) {
        ret = vof_seek(vof, args[0], args[1], args[2]);
    } else if (cmpserv("claim", 3, 1)) {
        ret = vof_claim(vof, args[0], args[1], args[2]);
        if (ret != -1) {
            vof_dt_memory_available(fdt, vof->claimed, vof->claimed_base);
        }
    } else if (cmpserv("release", 2, 0)) {
        ret = vof_release(vof, args[0], args[1]);
        if (ret != -1) {
            vof_dt_memory_available(fdt, vof->claimed, vof->claimed_base);
        }
    } else if (cmpserv("call-method", 0, 0)) {
        ret = vof_call_method(vof, args[0], args[1], args[2], args[3], args[4],
                              args[5], rets);
    } else if (cmpserv("interpret", 0, 0)) {
        ret = vof_call_interpret(args[0], args[1], args[2], rets);
    } else if (cmpserv("milliseconds", 0, 1)) {
        ret = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    } else if (cmpserv("quiesce", 0, 0)) {
        vof_quiesce(fdt, vof);
    } else if (cmpserv("exit", 0, 0)) {
        error_report("Stopped as the VM requested \"exit\"");
        vm_stop(RUN_STATE_PAUSED); /* Or qemu_system_guest_panicked(NULL); ? */
    } else {
        trace_vof_error_unknown_service(service, nargs, nrets);
        ret = -1;
    }

    return ret;
}

static void vof_instance_free(gpointer data)
{
    OfInstance *inst = (OfInstance *) data;

    g_free(inst->params);
    g_free(inst->path);
    g_free(inst);
}

void vof_init(Vof *vof, uint64_t top_addr, Error **errp)
{
    vof_cleanup(vof);

    vof->of_instances = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, vof_instance_free);
    vof->claimed = g_array_new(false, false, sizeof(OfClaimed));
    vof->top_addr = MIN(top_addr, 4 * GiB); /* Keep allocations in 32bit */

    if (vof_claim(vof, 0, vof->fw_size, 0) == -1) {
        error_setg(errp, "Memory for firmware is in use");
    }
}

void vof_cleanup(Vof *vof)
{
    if (vof->claimed) {
        g_array_unref(vof->claimed);
    }
    if (vof->of_instances) {
        g_hash_table_unref(vof->of_instances);
    }
    vof->claimed = NULL;
    vof->of_instances = NULL;
}

void vof_build_dt(void *fdt, Vof *vof)
{
    uint32_t phandle;
    int i, offset, proplen = 0;
    const void *prop;
    bool found = false;
    GArray *phandles = g_array_new(false, false, sizeof(uint32_t));

    /* Add "disk" nodes to SCSI hosts, same for "network" */
    for (offset = fdt_next_node(fdt, -1, NULL), phandle = 1;
         offset >= 0;
         offset = fdt_next_node(fdt, offset, NULL), ++phandle) {

        const char *nodename = fdt_get_name(fdt, offset, NULL);
        if (strncmp(nodename, "scsi@", 5) == 0 ||
            strncmp(nodename, "v-scsi@", 7) == 0) {
            int disk_node_off = fdt_add_subnode(fdt, offset, "disk");

            fdt_setprop_string(fdt, disk_node_off, "device_type", "block");
        }
    }

    /* Add options now, doing it at the end of this __func__ breaks it :-/ */
    offset = fdt_add_subnode(fdt, 0, "options");
    if (offset > 0) {
        struct winsize ws;

        if (ioctl(1, TIOCGWINSZ, &ws) != -1) {
            _FDT(fdt_setprop_cell(fdt, offset, "screen-#columns", ws.ws_col));
            _FDT(fdt_setprop_cell(fdt, offset, "screen-#rows", ws.ws_row));
        }
        _FDT(fdt_setprop_cell(fdt, offset, "real-mode?", 1));
    }

    /* Find all predefined phandles */
    for (offset = fdt_next_node(fdt, -1, NULL);
         offset >= 0;
         offset = fdt_next_node(fdt, offset, NULL)) {
        prop = fdt_getprop(fdt, offset, "phandle", &proplen);
        if (prop && proplen == sizeof(uint32_t)) {
            phandle = fdt32_ld(prop);
            g_array_append_val(phandles, phandle);
        }
    }

    /* Assign phandles skipping the predefined ones */
    for (offset = fdt_next_node(fdt, -1, NULL), phandle = 1;
         offset >= 0;
         offset = fdt_next_node(fdt, offset, NULL), ++phandle) {
        prop = fdt_getprop(fdt, offset, "phandle", &proplen);
        if (prop) {
            continue;
        }
        /* Check if the current phandle is not allocated already */
        for ( ; ; ++phandle) {
            for (i = 0, found = false; i < phandles->len; ++i) {
                if (phandle == g_array_index(phandles, uint32_t, i)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
        }
        _FDT(fdt_setprop_cell(fdt, offset, "phandle", phandle));
    }
    g_array_unref(phandles);

    vof_dt_memory_available(fdt, vof->claimed, vof->claimed_base);
}

static const TypeInfo vof_machine_if_info = {
    .name = TYPE_VOF_MACHINE_IF,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(VofMachineIfClass),
};

static void vof_machine_if_register_types(void)
{
    type_register_static(&vof_machine_if_info);
}
type_init(vof_machine_if_register_types)
