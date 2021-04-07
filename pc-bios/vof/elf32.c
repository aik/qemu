/*
 * Copyright (c) 2004, 2011 IBM Corporation
 * All rights reserved.
 * This program and the accompanying materials
 * are made available under the terms of the BSD License
 * which accompanies this distribution, and is available at
 * http://www.opensource.org/licenses/bsd-license.php
 *
 * Contributors:
 *     IBM Corporation - initial implementation
 */

/*
 * 32-bit ELF loader
 */
#include "vof.h"

/* ELF object file types */
#define ET_NONE        0    /* No file type */
#define ET_REL        1    /* Relocatable file */
#define ET_EXEC        2    /* Executable file */
#define ET_DYN        3    /* Shared object file */
#define ET_CORE        4    /* Core file */

/* ELF object endian */
#define ELFDATA2LSB    1    /* 2's complement, little endian */
#define ELFDATA2MSB    2    /* 2's complement, big endian */

struct ehdr32 {
    uint32_t ei_ident;
    uint8_t ei_class;
    uint8_t ei_data;
    uint8_t ei_version;
    uint8_t ei_pad[9];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct phdr32 {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

static inline void bswap_16p(uint16_t *x)
{
    *x = __builtin_bswap16(*x);
}

static inline void bswap_32p(uint32_t *x)
{
    *x = __builtin_bswap32(*x);
}

static inline void bswap_64p(uint64_t *x)
{
    *x = __builtin_bswap64(*x);
}

static struct phdr32 *get_phdr32(void *file_addr)
{
    return (struct phdr32 *) (((unsigned char *)file_addr)
                              + ((struct ehdr32 *)file_addr)->e_phoff);
}

static void
load_segment(void *file_addr, struct phdr32 *phdr, signed long offset,
             int (*pre_load)(void*, long),
             void (*post_load)(void*, long))
{
    unsigned long src = phdr->p_offset + (unsigned long) file_addr;
    unsigned long destaddr;

    destaddr = (unsigned long)phdr->p_paddr;
    destaddr = destaddr + offset;

    /* check if we're allowed to copy */
    if (pre_load != NULL) {
        if (pre_load((void *)destaddr, phdr->p_memsz) != 0) {
            return;
        }
    }

    /* copy into storage */
    memmove((void *)destaddr, (void *)src, phdr->p_filesz);

    /* clear bss */
    memset((void *)(destaddr + phdr->p_filesz), 0,
           phdr->p_memsz - phdr->p_filesz);

    if (phdr->p_memsz && post_load) {
        post_load((void *)destaddr, phdr->p_memsz);
    }
}

unsigned int
elf_load_segments32(void *file_addr, signed long offset,
                    int (*pre_load)(void*, long),
                    void (*post_load)(void*, long))
{
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;
    /* Calculate program header address */
    struct phdr32 *phdr = get_phdr32(file_addr);
    int i;

    /* loop e_phnum times */
    for (i = 0; i <= ehdr->e_phnum; i++) {
        /* PT_LOAD ? */
        if (phdr->p_type == 1) {
            if (phdr->p_paddr != phdr->p_vaddr) {
                return 0;
            }

            /* copy segment */
            load_segment(file_addr, phdr, offset, pre_load,
                         post_load);
        }
        /* step to next header */
        phdr = (struct phdr32 *)(((uint8_t *)phdr) + ehdr->e_phentsize);
    }

    /*
     * Entry point is always a virtual address, so translate it
     * to physical before returning it
     */
    return ehdr->e_entry;
}

/**
 * Return the base address for loading (i.e. the address of the first PT_LOAD
 * segment)
 * @param  file_addr    pointer to the ELF file in memory
 * @return        the base address
 */
    long
elf_get_base_addr32(void *file_addr)
{
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;
    struct phdr32 *phdr = get_phdr32(file_addr);
    int i;

    /* loop e_phnum times */
    for (i = 0; i <= ehdr->e_phnum; i++) {
        /* PT_LOAD ? */
        if (phdr->p_type == 1) {
            return phdr->p_paddr;
        }
        /* step to next header */
        phdr = (struct phdr32 *)(((uint8_t *)phdr) + ehdr->e_phentsize);
    }

    return 0;
}

uint32_t elf_get_eflags_32(void *file_addr)
{
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;

    return ehdr->e_flags;
}

    void
elf_byteswap_header32(void *file_addr)
{
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;
    struct phdr32 *phdr;
    int i;

    bswap_16p(&ehdr->e_type);
    bswap_16p(&ehdr->e_machine);
    bswap_32p(&ehdr->e_version);
    bswap_32p(&ehdr->e_entry);
    bswap_32p(&ehdr->e_phoff);
    bswap_32p(&ehdr->e_shoff);
    bswap_32p(&ehdr->e_flags);
    bswap_16p(&ehdr->e_ehsize);
    bswap_16p(&ehdr->e_phentsize);
    bswap_16p(&ehdr->e_phnum);
    bswap_16p(&ehdr->e_shentsize);
    bswap_16p(&ehdr->e_shnum);
    bswap_16p(&ehdr->e_shstrndx);

    phdr = get_phdr32(file_addr);

    /* loop e_phnum times */
    for (i = 0; i <= ehdr->e_phnum; i++) {
        bswap_32p(&phdr->p_type);
        bswap_32p(&phdr->p_offset);
        bswap_32p(&phdr->p_vaddr);
        bswap_32p(&phdr->p_paddr);
        bswap_32p(&phdr->p_filesz);
        bswap_32p(&phdr->p_memsz);
        bswap_32p(&phdr->p_flags);
        bswap_32p(&phdr->p_align);

        /* step to next header */
        phdr = (struct phdr32 *)(((uint8_t *)phdr) + ehdr->e_phentsize);
    }
}

/**
 * elf_check_file tests if the file at file_addr is
 * a correct endian, ELF PPC executable
 * @param file_addr  pointer to the start of the ELF file
 * @return           the class (1 for 32 bit, 2 for 64 bit)
 *                   -1 if it is not an ELF file
 *                   -2 if it has the wrong endianness
 *                   -3 if it is not an ELF executable
 *                   -4 if it is not for PPC
 */
    static int
elf_check_file(unsigned long *file_addr)
{
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;

    /* check if it is an ELF image at all */
    if (ehdr->ei_ident != 0x7f454c46) {
        return -1;
    }

    if (ELFDATA2MSB != ehdr->ei_data) {
        switch (ehdr->ei_class) {
        case 1:
            elf_byteswap_header32(file_addr);
            break;
        }
    }

    /*
     * check if it is an ELF executable ... and also
     * allow DYN files, since this is specified by ePAPR
     */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        return -3;
    }

    /* check if it is a PPC ELF executable */
    if (ehdr->e_machine != 0x14 && ehdr->e_machine != 0x15) {
        return -4;
    }
    return ehdr->ei_class;
}

int elf_load_file(void *file_addr, uint32_t *entry,
                  int (*pre_load)(void*, long),
                  void (*post_load)(void*, long))
{
    int type = elf_check_file(file_addr);
    struct ehdr32 *ehdr = (struct ehdr32 *) file_addr;

    switch (type) {
    case 1:
        *entry = elf_load_segments32(file_addr, 0, pre_load, post_load);
        if (ehdr->ei_data != ELFDATA2MSB) {
            type = 5; /* LE32 ABIv1 */
        }
        break;
    }
    if (*entry == 0) {
        type = 0;
    }

    return type;
}
