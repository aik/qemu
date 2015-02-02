#ifndef VFIO_API_H
#define VFIO_API_H

#include "qemu/typedefs.h"

extern int vfio_container_ioctl(AddressSpace *as,
                                int req, void *param);

extern int vfio_container_spapr_set_liobn(AddressSpace *as,
                                          uint64_t liobn,
                                          uint64_t start_addr);

#endif
