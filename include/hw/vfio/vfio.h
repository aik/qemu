#ifndef HW_VFIO_H
#define HW_VFIO_H

bool vfio_eeh_as_ok(AddressSpace *as);
int vfio_eeh_as_op(AddressSpace *as, uint32_t op);
int vfio_ibm_npu2_context(AddressSpace *as, struct PCIDevice *pdev,
        unsigned op, unsigned contextid, unsigned msrhi, unsigned msrlo);
unsigned long vfio_pci_get_dev_id(struct PCIDevice *pdev);

#endif
