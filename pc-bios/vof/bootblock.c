#include "vof.h"

#define UUID_FMT_LEN 36
#define UUID_FMT "%02hhx%02hhx%02hhx%02hhx-" \
    "%02hhx%02hhx-%02hhx%02hhx-" \
    "%02hhx%02hhx-" \
    "%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx"

typedef struct {
    union {
        unsigned char data[16];
        struct {
            /* Generated in BE endian, can be swapped with qemu_uuid_bswap. */
            uint32_t time_low;
            uint16_t time_mid;
            uint16_t time_high_and_version;
            uint8_t  clock_seq_and_reserved;
            uint8_t  clock_seq_low;
            uint8_t  node[6];
        } fields;
    };
} UUID;

struct gpt_header {
    char signature[8];
    char revision[4];
    uint32_t header_size;
    uint32_t crc;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    char guid[16];
    uint64_t partition_entries_lba;
    uint32_t nr_partition_entries;
    uint32_t size_partition_entry;
    uint32_t crc_partitions;
};

#define GPT_SIGNATURE "EFI PART"
#define GPT_REVISION "\0\0\1\0" /* revision 1.0 */

struct gpt_entry {
    char partition_type_guid[16];
    char unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    char name[72];                /* UTF-16LE */
};

#define GPT_MIN_PARTITIONS 128
#define GPT_PT_ENTRY_SIZE 128
#define SECTOR_SIZE 512

static int find_prep_partition_on_gpt(ihandle blk, uint8_t *lba01,
                                      uint64_t *offset, uint64_t *size)
{
    unsigned long i, partnum, partentrysize;
    int ret;
    struct gpt_header *hdr = (struct gpt_header *) (lba01 + SECTOR_SIZE);
    UUID prep_uuid = { .fields = { 0x382d1a9e, 0x12c6, 0x1643, 0xaa, 0x26,
        { 0x8b, 0x49, 0x52, 0x1e, 0x5a, 0x8b} } };

    if (memcmp(hdr, "EFI PART", 8)) {
        return -1;
    }

    partnum = le32_to_cpu(hdr->nr_partition_entries);
    partentrysize = le32_to_cpu(hdr->size_partition_entry);

    if (partentrysize < 128 || partentrysize > 512) {
        return -1;
    }

    for (i = 0; i < partnum; ++i) {
        uint8_t partdata[partentrysize];
        struct gpt_entry *entry = (struct gpt_entry *) partdata;
        uint64_t first, last;
        UUID parttype;
        char uuid[UUID_FMT_LEN + 1];

        ci_seek(blk, 2 * SECTOR_SIZE + i * partentrysize);
        ret = ci_read(blk, partdata, sizeof(partdata));
        if (ret < 0) {
            return ret;
        } else if (!ret) {
            return -1;
        }

        memcpy(parttype.data, entry->partition_type_guid, 16);
        first = le64_to_cpu(entry->first_lba);
        last = le64_to_cpu(entry->last_lba);

        if (!memcmp(&parttype, &prep_uuid, sizeof(parttype))) {
            *offset = first * SECTOR_SIZE;
            *size = (last - first) * SECTOR_SIZE;
        }
    }

    if (*offset) {
        return 0;
    }

    return -1;
}

struct partition_record {
    uint8_t bootable;
    uint8_t start_head;
    uint32_t start_cylinder;
    uint8_t start_sector;
    uint8_t system;
    uint8_t end_head;
    uint8_t end_cylinder;
    uint8_t end_sector;
    uint32_t start_sector_abs;
    uint32_t nb_sectors_abs;
};

static void read_partition(uint8_t *p, struct partition_record *r)
{
    r->bootable = p[0];
    r->start_head = p[1];
    r->start_cylinder = p[3] | ((p[2] << 2) & 0x0300);
    r->start_sector = p[2] & 0x3f;
    r->system = p[4];
    r->end_head = p[5];
    r->end_cylinder = p[7] | ((p[6] << 2) & 0x300);
    r->end_sector = p[6] & 0x3f;
    r->start_sector_abs = le32_to_cpu(*(uint32_t *)(p + 8));
    r->nb_sectors_abs   = le32_to_cpu(*(uint32_t *)(p + 12));
}

static int find_prep_partition(ihandle blk, uint64_t *offset, uint64_t *size)
{
    uint8_t lba01[SECTOR_SIZE * 2];
    int i;
    int ret = -1;

    ci_seek(blk, 0);
    ret = ci_read(blk, lba01, sizeof(lba01));
    if (ret < 0) {
        return ret;
    }

    if (lba01[510] != 0x55 || lba01[511] != 0xaa) {
        return find_prep_partition_on_gpt(blk, lba01, offset, size);
    }

    for (i = 0; i < 4; i++) {
        struct partition_record part;

        read_partition(&lba01[446 + 16 * i], &part);

        if (!part.system || !part.nb_sectors_abs) {
            continue;
        }

        /* 0xEE == GPT */
        if (part.system == 0xEE) {
            ret = find_prep_partition_on_gpt(blk, lba01, offset, size);
        }
        /* 0x41 == PReP */
        if (part.system == 0x41) {
            *offset = part.start_sector_abs;
            *offset <<= 9;
            *size = (uint64_t)part.nb_sectors_abs << 9;
            ret = 0;
        }
    }

    return ret;
}

static int elf_pre_load(void *destaddr, long size)
{
    void *ret = ci_claim(destaddr, size, 0);

    return (ret == destaddr) ? 0 : -1;
}

static void try_boot_block_device(ihandle blk, const char *path)
{
    uint32_t rc, elf_addr = 0;
    uint64_t offset = 0, size = 0;
    void *grub;
    int ret;

    if (find_prep_partition(blk, &offset, &size)) {
        return;
    }

    grub = ci_claim((void *)0x20000000, size, 0);
    if (!grub) {
        return;
    }

    ci_seek(blk, offset);
    rc = ci_read(blk, grub, size);
    if (rc <= 0) {
        ci_release(grub, size);
        return;
    }

    ret = elf_load_file(grub, &elf_addr, elf_pre_load, NULL);
    ci_release(grub, size);
    if (ret < 0) {
        return;
    }

    ci_stdout("** Booting from ");
    ci_stdout(path);
    ci_stdout("\n");
    ci_stdout("AIKDBG\n");
    do_boot(elf_addr, 0, 0);
}

void boot_block(void)
{
    char bootlist[2048], *cur, *next;
    uint32_t cb, blk;
    phandle chosen = ci_finddevice("/chosen");

    cb = ci_getprop(chosen, "qemu,boot-list", bootlist,
                    sizeof(bootlist) - 1);
    bootlist[sizeof(bootlist) - 1] = '\0';

    if (strlen(bootlist) == 0) {
        return;
    }

    for (cur = bootlist; cb > 0; cur = next + 1) {
        for (next = cur; cb > 0; --cb) {
            if (*next == '\n') {
                *next = '\0';
                ++next;
                --cb;
                break;
            }
        }

        blk = ci_open(cur);
        if (!blk) {
            continue;
        }

        try_boot_block_device(blk, cur);
        ci_close(blk);
    }
}
