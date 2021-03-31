/*
 * Virtual Open Firmware
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
//#include <stdint.h>

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long uint32_t;
typedef unsigned long long uint64_t;
#define NULL (0)
#define PROM_ERROR (-1u)
typedef unsigned long ihandle;
typedef unsigned long phandle;
typedef int size_t;
typedef void client(void);

//#include "libelf.h"

#define __stringify(x)	#x
#define MYLOC(l)	("--line=" __stringify(l))

/* globals */
extern void _prom_entry(void); /* OF CI entry point (i.e. this firmware) */

void do_boot(unsigned long addr, unsigned long r3, unsigned long r4);

/* libc */
int strlen(const char *s);
int strcmp(const char *s1, const char *s2);
void *memcpy(void *dest, const void *src, size_t n);
int memcmp(const void *ptr1, const void *ptr2, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t size);
int snprintf(char *buf, int len, const char *fmt, ...);
int printk(const char *fmt, ...);

/* CI wrappers */
//void ci_init(void);
void ci_panic(const char *str);
ihandle ci_open(const char *path);
void ci_close(ihandle ih);
uint32_t ci_block_size(ihandle ih);
uint32_t ci_seek(ihandle ih, uint64_t offset);
uint32_t ci_read(ihandle ih, void *buf, int len);
uint32_t ci_write(ihandle ih, const void *buf, int len);
void *ci_claim(void *virt, uint32_t size, uint32_t align);
uint32_t ci_release(void *virt, uint32_t size);
phandle ci_finddevice(const char *path);
uint32_t ci_getprop(phandle ph, const char *propname, void *prop, int len);
void ci_stdout(const char *buf);
void ci_stdoutn(const char *buf, int len);

/* ELF */
int elf_load_file(void *file_addr, uint32_t *entry,
                  int (*pre_load)(void*, long),
                  void (*post_load)(void*, long));

/* booting from blockdev */
void boot_block(void);

/* booting from -kernel */
void boot_from_memory(uint64_t initrd, uint64_t initrdsize);

/* Entry points for CI and RTAS */
extern uint32_t ci_entry(uint32_t params);
extern unsigned long hv_rtas(unsigned long params);
extern unsigned int hv_rtas_size;

/* bswap */
#define le16_to_cpu(x) __builtin_bswap16(x)
#define le32_to_cpu(x) __builtin_bswap32(x)
#define le64_to_cpu(x) __builtin_bswap64(x)
