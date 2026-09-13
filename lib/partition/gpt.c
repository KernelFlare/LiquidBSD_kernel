/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "partition_priv.h"

#include <arch/defines.h>
#include <endian.h>
#include <lib/bio.h>
#include <lib/cksum.h>
#include <lk/debug.h>
#include <lk/err.h>
#include <lk/pow2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Bounds the entry array, which is read and checksummed in one piece. The usual table is
// 128 entries of 128 bytes.
#define GPT_ENTRY_ARRAY_MAX (256 * 1024)

#define GPT_ATTR_REQUIRED        (1ULL << 0)
#define GPT_ATTR_NO_BLOCK_IO     (1ULL << 1)
#define GPT_ATTR_LEGACY_BOOTABLE (1ULL << 2)

static const struct {
    uint8_t guid[16];
    const char *name;
} gpt_type_names[] = {
    { GPT_GUID(0xc12a7328, 0xf81f, 0x11d2, 0xba4b, 0x00a0c93ec93bULL), "EFI system" },
    { GPT_GUID(0x21686148, 0x6449, 0x6e6f, 0x744e, 0x656564454649ULL), "BIOS boot" },
    { GPT_GUID(0xebd0a0a2, 0xb9e5, 0x4433, 0x87c0, 0x68b6b72699c7ULL), "Microsoft basic data" },
    { GPT_GUID(0x0fc63daf, 0x8483, 0x4772, 0x8e79, 0x3d69d8477de4ULL), "Linux filesystem" },
    { GPT_GUID(0x0657fd6d, 0xa4ab, 0x43c4, 0x84e5, 0x0933c84b4f4fULL), "Linux swap" },
    { GPT_GUID(0xe6d6d379, 0xf507, 0x44c2, 0xa23c, 0x238f2a3df928ULL), "Linux LVM" },
    { GPT_GUID(0xa19d880f, 0x05fc, 0x4d3b, 0xa006, 0x743f0f84911eULL), "Linux RAID" },
};

static const uint8_t gpt_unused_guid[16];

// One of the two copies of the table, index 0 the primary and 1 the backup.
struct gpt_table {
    uint64_t lba;
    bool loaded;     // false if the backup was not needed
    status_t status; // of the load
    const char *why; // set when status is an error
    struct gpt_header hdr;
    uint8_t *entries; // hdr.num_entries entries when status is NO_ERROR
};

void gpt_format_guid(char *buf, size_t len, const uint8_t *g) {
    snprintf(buf, len, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", g[3],
             g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13],
             g[14], g[15]);
}

static void gpt_header_to_host(struct gpt_header *hdr) {
    hdr->revision = LE32(hdr->revision);
    hdr->header_size = LE32(hdr->header_size);
    hdr->header_crc32 = LE32(hdr->header_crc32);
    hdr->my_lba = LE64(hdr->my_lba);
    hdr->alternate_lba = LE64(hdr->alternate_lba);
    hdr->first_usable_lba = LE64(hdr->first_usable_lba);
    hdr->last_usable_lba = LE64(hdr->last_usable_lba);
    hdr->entries_lba = LE64(hdr->entries_lba);
    hdr->num_entries = LE32(hdr->num_entries);
    hdr->entry_size = LE32(hdr->entry_size);
    hdr->entries_crc32 = LE32(hdr->entries_crc32);
}

// Checks the header in blk, one block read from lba, and returns it in host order.
// Zeroes the header's crc field in blk.
static status_t gpt_parse_header(const bdev_t *dev, uint8_t *blk, uint64_t lba,
                                 struct gpt_header *hdr, const char **why) {
    memcpy(hdr, blk, sizeof(*hdr));
    if (memcmp(hdr->signature, GPT_SIGNATURE, sizeof(hdr->signature)) != 0) {
        *why = "no signature";
        return ERR_NOT_FOUND;
    }
    gpt_header_to_host(hdr);

    if (hdr->header_size < GPT_HEADER_MIN_SIZE || hdr->header_size > dev->block_size) {
        *why = "bad header size";
        return ERR_NOT_VALID;
    }
    memset(blk + offsetof(struct gpt_header, header_crc32), 0, sizeof(hdr->header_crc32));
    if ((uint32_t)crc32(0, blk, hdr->header_size) != hdr->header_crc32) {
        *why = "bad header crc";
        return ERR_CRC_FAIL;
    }
    if (hdr->my_lba != lba) {
        *why = "header's own lba does not match its location";
        return ERR_NOT_VALID;
    }

    // Every partition is checked against this range, and block_count is a bnum_t, so the
    // LBAs of a partition that passes fit in one too.
    if (hdr->first_usable_lba > hdr->last_usable_lba || hdr->last_usable_lba >= dev->block_count) {
        *why = "usable range is empty or past the end of the device";
        return ERR_NOT_VALID;
    }

    if (hdr->entry_size < GPT_ENTRY_MIN_SIZE || !ispow2(hdr->entry_size) || hdr->num_entries == 0) {
        *why = "bad entry count or size";
        return ERR_NOT_VALID;
    }
    uint64_t array_bytes = (uint64_t)hdr->num_entries * hdr->entry_size;
    if (array_bytes > GPT_ENTRY_ARRAY_MAX) {
        *why = "entry array too large";
        return ERR_NOT_VALID;
    }

    // the array has to be on the device, clear of LBA 0, this header and the usable area
    uint64_t array_end =
        hdr->entries_lba + ((array_bytes + dev->block_size - 1) >> dev->block_shift);
    if (hdr->entries_lba == 0 || array_end > dev->block_count ||
        (lba >= hdr->entries_lba && lba < array_end) ||
        (array_end > hdr->first_usable_lba && hdr->entries_lba <= hdr->last_usable_lba)) {
        *why = "entry array overlaps the header, the usable range or the device end";
        return ERR_NOT_VALID;
    }

    return NO_ERROR;
}

// Reads and checks the table whose header is at lba. On success *entries is a heap buffer
// holding hdr->num_entries entries, which the caller frees.
static status_t gpt_load(bdev_t *dev, uint8_t *blk, uint64_t lba, struct gpt_header *hdr,
                         uint8_t **entries, const char **why) {
    ssize_t err = bio_read_block(dev, blk, (bnum_t)lba, 1);
    if (err != (ssize_t)dev->block_size) {
        *why = "header read failed";
        return (err < 0) ? (status_t)err : ERR_IO;
    }

    status_t status = gpt_parse_header(dev, blk, lba, hdr, why);
    if (status < 0) {
        return status;
    }

    size_t array_bytes = (size_t)hdr->num_entries * hdr->entry_size;
    size_t read_len = ROUNDUP(array_bytes, dev->block_size);
    uint8_t *buf = memalign(CACHE_LINE, ROUNDUP(read_len, CACHE_LINE));
    if (!buf) {
        *why = "out of memory";
        return ERR_NO_MEMORY;
    }

    err = bio_read(dev, buf, (off_t)(hdr->entries_lba << dev->block_shift), read_len);
    if (err != (ssize_t)read_len) {
        free(buf);
        *why = "entry array read failed";
        return (err < 0) ? (status_t)err : ERR_IO;
    }
    if ((uint32_t)crc32(0, buf, array_bytes) != hdr->entries_crc32) {
        free(buf);
        *why = "bad entry array crc";
        return ERR_CRC_FAIL;
    }

    *entries = buf;
    return NO_ERROR;
}

// Loads the primary table and, if all is set or the primary is unusable, the backup. The
// backup's location comes from the device size, not from a primary that failed. Returns the
// index of the table to use, or -1 if neither is usable.
static int gpt_load_tables(bdev_t *dev, struct gpt_table tables[2], bool all) {
    const uint64_t lbas[2] = { 1, dev->block_count - 1 };
    uint8_t *blk = memalign(CACHE_LINE, ROUNDUP(dev->block_size, CACHE_LINE));
    int use = -1;
    for (int i = 0; i < 2; i++) {
        struct gpt_table *t = &tables[i];
        memset(t, 0, sizeof(*t));
        t->lba = lbas[i];
        if (use >= 0 && !all) {
            continue;
        }
        t->loaded = true;
        if (!blk) {
            t->status = ERR_NO_MEMORY;
            t->why = "out of memory";
            continue;
        }
        t->status = gpt_load(dev, blk, t->lba, &t->hdr, &t->entries, &t->why);
        if (t->status == NO_ERROR && use < 0) {
            use = i;
        }
    }
    free(blk);
    return use;
}

static void gpt_free_tables(struct gpt_table tables[2]) {
    free(tables[0].entries);
    free(tables[1].entries);
}

static void gpt_get_entry(const struct gpt_table *table, uint32_t slot, struct gpt_entry *entry) {
    memcpy(entry, table->entries + (size_t)slot * table->hdr.entry_size, sizeof(*entry));
}

// Returns NULL if the entry in slot can be published, otherwise why not.
static const char *gpt_entry_problem(const struct gpt_header *hdr, uint32_t slot, uint64_t first,
                                     uint64_t last) {
    if (slot >= PARTITION_MAX_SLOTS) {
        return "slot is past the publish limit";
    }
    if (first > last) {
        return "ends before it starts";
    }
    if (first < hdr->first_usable_lba || last > hdr->last_usable_lba) {
        return "outside the usable range";
    }
    return NULL;
}

// Folds the UTF-16LE partition name to printable ASCII.
static void gpt_entry_name(const struct gpt_entry *entry, char *out, size_t len) {
    size_t i;
    for (i = 0; i < countof(entry->name) && i + 1 < len; i++) {
        uint16_t c = LE16(entry->name[i]);
        if (c == 0) {
            break;
        }
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[i] = '\0';
}

static const char *gpt_type_name(const uint8_t *guid) {
    for (size_t i = 0; i < countof(gpt_type_names); i++) {
        if (memcmp(gpt_type_names[i].guid, guid, sizeof(gpt_type_names[i].guid)) == 0) {
            return gpt_type_names[i].name;
        }
    }
    return "unknown type";
}

int gpt_publish(bdev_t *dev, const char *device) {
    if (dev->block_count < 3 || dev->block_size < 512) {
        return ERR_NOT_FOUND;
    }

    struct gpt_table tables[2];
    int use = gpt_load_tables(dev, tables, false);
    for (int i = 0; i < 2; i++) {
        if (tables[i].loaded && tables[i].status != NO_ERROR) {
            dprintf(INFO, "gpt: %s table on %s: %s\n", i ? "backup" : "primary", device,
                    tables[i].why);
        }
    }
    if (use < 0) {
        gpt_free_tables(tables);
        return ERR_NOT_FOUND;
    }
    if (use > 0) {
        dprintf(INFO, "gpt: using the backup table on %s\n", device);
    }

    const struct gpt_table *table = &tables[use];
    int count = 0;
    for (uint32_t i = 0; i < table->hdr.num_entries; i++) {
        struct gpt_entry entry;
        gpt_get_entry(table, i, &entry);
        if (memcmp(entry.type_guid, gpt_unused_guid, sizeof(gpt_unused_guid)) == 0) {
            continue;
        }

        uint64_t first = LE64(entry.first_lba);
        uint64_t last = LE64(entry.last_lba);
        const char *problem = gpt_entry_problem(&table->hdr, i, first, last);
        if (problem) {
            dprintf(INFO, "gpt: not publishing slot %u on %s: %s\n", i, device, problem);
            continue;
        }

        char name[countof(entry.name) + 1];
        gpt_entry_name(&entry, name, sizeof(name));
        dprintf(INFO, "gpt: %u: start %llu, len %llu, name '%s'\n", i, (unsigned long long)first,
                (unsigned long long)(last - first + 1), name);

        char subdevice[128];
        snprintf(subdevice, sizeof(subdevice), "%sp%u", device, i);
        status_t err =
            bio_publish_subdevice(device, subdevice, (bnum_t)first, (bnum_t)(last - first + 1));
        if (err < 0) {
            dprintf(INFO, "error publishing subdevice '%s'\n", subdevice);
            continue;
        }
        count++;
    }

    gpt_free_tables(tables);
    return count;
}

void gpt_dump(bdev_t *dev, bool protective) {
    if (dev->block_count < 3 || dev->block_size < 512) {
        printf("GPT: device is too small to hold one\n");
        return;
    }

    struct gpt_table tables[2];
    int use = gpt_load_tables(dev, tables, true);
    if (use < 0 && tables[0].status == ERR_NOT_FOUND && tables[1].status == ERR_NOT_FOUND) {
        printf("GPT: none\n");
        gpt_free_tables(tables);
        return;
    }

    printf("GPT:\n");
    for (int i = 0; i < 2; i++) {
        printf("  %s header at lba %llu: %s\n", i ? "backup" : "primary",
               (unsigned long long)tables[i].lba,
               tables[i].status == NO_ERROR ? "valid" : tables[i].why);
    }
    if (use < 0) {
        gpt_free_tables(tables);
        return;
    }

    const struct gpt_table *table = &tables[use];
    const struct gpt_header *hdr = &table->hdr;
    const struct gpt_table *other = &tables[1 - use];
    if (!protective) {
        printf("  warning: no protective MBR entry, partition_publish() ignores this table\n");
    }
    if (hdr->alternate_lba != other->lba) {
        printf("  warning: alternate lba is %llu, expected %llu\n",
               (unsigned long long)hdr->alternate_lba, (unsigned long long)other->lba);
    }
    if (other->status == NO_ERROR &&
        (other->hdr.entries_crc32 != hdr->entries_crc32 ||
         memcmp(other->hdr.disk_guid, hdr->disk_guid, sizeof(hdr->disk_guid)) != 0)) {
        printf("  warning: primary and backup tables differ\n");
    }

    char guid[GPT_GUID_STR_LEN];
    gpt_format_guid(guid, sizeof(guid), hdr->disk_guid);
    printf("  using the %s: revision %u.%u, header size %u, header crc 0x%08x\n",
           use ? "backup" : "primary", hdr->revision >> 16, hdr->revision & 0xffff,
           hdr->header_size, hdr->header_crc32);
    printf("  disk guid %s\n", guid);
    printf("  usable lba %llu-%llu, entries at lba %llu: %u x %u bytes, crc 0x%08x\n",
           (unsigned long long)hdr->first_usable_lba, (unsigned long long)hdr->last_usable_lba,
           (unsigned long long)hdr->entries_lba, hdr->num_entries, hdr->entry_size,
           hdr->entries_crc32);

    printf("  %4s %12s %12s %12s %10s  %s\n", "slot", "first lba", "last lba", "blocks", "size",
           "type, name");
    uint32_t used = 0;
    for (uint32_t i = 0; i < hdr->num_entries; i++) {
        struct gpt_entry entry;
        gpt_get_entry(table, i, &entry);
        if (memcmp(entry.type_guid, gpt_unused_guid, sizeof(gpt_unused_guid)) == 0) {
            continue;
        }
        used++;

        uint64_t first = LE64(entry.first_lba);
        uint64_t last = LE64(entry.last_lba);
        uint64_t blocks = (first <= last) ? last - first + 1 : 0;
        char size[16];
        partition_format_size(size, sizeof(size), blocks << dev->block_shift);
        char name[countof(entry.name) + 1];
        gpt_entry_name(&entry, name, sizeof(name));
        printf("  %4u %12llu %12llu %12llu %10s  %s, '%s'\n", i, (unsigned long long)first,
               (unsigned long long)last, (unsigned long long)blocks, size,
               gpt_type_name(entry.type_guid), name);

        gpt_format_guid(guid, sizeof(guid), entry.type_guid);
        printf("       type guid   %s\n", guid);
        gpt_format_guid(guid, sizeof(guid), entry.unique_guid);
        printf("       unique guid %s\n", guid);
        uint64_t attrs = LE64(entry.attributes);
        printf("       attributes  0x%016llx%s%s%s\n", (unsigned long long)attrs,
               (attrs & GPT_ATTR_REQUIRED) ? " required" : "",
               (attrs & GPT_ATTR_NO_BLOCK_IO) ? " no-block-io" : "",
               (attrs & GPT_ATTR_LEGACY_BOOTABLE) ? " legacy-bios-bootable" : "");

        const char *problem = gpt_entry_problem(hdr, i, first, last);
        if (problem) {
            printf("       not published: %s\n", problem);
        }
    }
    printf("  %u of %u slots in use\n", used, hdr->num_entries);

    gpt_free_tables(tables);
}
