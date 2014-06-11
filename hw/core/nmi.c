/*
 *  NMI monitor handler class and helpers.
 *
 *  Copyright (c) 2014 Alexey Kardashevskiy <aik@ozlabs.ru>
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

#include "hw/nmi.h"
#include "qapi/qmp/qerror.h"

struct do_nmi_s {
    int cpu_index;
    Error *errp;
    bool handled;
};

static void nmi_children(Object *o, struct do_nmi_s *ns);

static int do_nmi(Object *o, void *opaque)
{
    struct do_nmi_s *ns = opaque;
    NMIState *n = (NMIState *) object_dynamic_cast(o, TYPE_NMI);

    if (n) {
        NMIClass *nc = NMI_GET_CLASS(n);

        ns->handled = true;
        nc->nmi_monitor_handler(n, ns->cpu_index, &ns->errp);
        if (ns->errp) {
            return -1;
        }
    }
    nmi_children(o, ns);

    return 0;
}

void nmi_children(Object *o, struct do_nmi_s *ns)
{
    object_child_foreach(o, do_nmi, ns);
}

void nmi_monitor_handle(int cpu_index, Error **errp)
{
    struct do_nmi_s ns = {
        .cpu_index = cpu_index,
        .errp = NULL,
        .handled = false
    };

    nmi_children(object_get_root(), &ns);
    if (ns.handled) {
        error_propagate(errp, ns.errp);
    } else {
        error_set(errp, QERR_UNSUPPORTED);
    }
}

static const TypeInfo nmi_info = {
    .name          = TYPE_NMI,
    .parent        = TYPE_INTERFACE,
    .class_size    = sizeof(NMIClass),
};

static void nmi_register_types(void)
{
    type_register_static(&nmi_info);
}

type_init(nmi_register_types)
