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
#include <lk/trace.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_TRACE 0

// Bounds the entry array, which is read and checksummed in one piece. The usual table is
// 128 entries of 128 bytes.
#define GPT_ENTRY_ARRAY_MAX (256 * 1024)

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
                                 struct gpt_header *hdr) {
    memcpy(hdr, blk, sizeof(*hdr));
    if (memcmp(hdr->signature, GPT_SIGNATURE, sizeof(hdr->signature)) != 0) {
        LTRACEF("no signature at lba %llu\n", (unsigned long long)lba);
        return ERR_NOT_FOUND;
    }
    gpt_header_to_host(hdr);

    if (hdr->header_size < GPT_HEADER_MIN_SIZE || hdr->header_size > dev->block_size) {
        LTRACEF("bad header size %u\n", hdr->header_size);
        return ERR_NOT_VALID;
    }
    memset(blk + offsetof(struct gpt_header, header_crc32), 0, sizeof(hdr->header_crc32));
    if ((uint32_t)crc32(0, blk, hdr->header_size) != hdr->header_crc32) {
        LTRACEF("bad header crc at lba %llu\n", (unsigned long long)lba);
        return ERR_CRC_FAIL;
    }
    if (hdr->my_lba != lba) {
        LTRACEF("header at lba %llu claims lba %llu\n", (unsigned long long)lba,
                (unsigned long long)hdr->my_lba);
        return ERR_NOT_VALID;
    }

    // Every partition is checked against this range, and block_count is a bnum_t, so the
    // LBAs of a partition that passes fit in one too.
    if (hdr->first_usable_lba > hdr->last_usable_lba || hdr->last_usable_lba >= dev->block_count) {
        LTRACEF("bad usable range\n");
        return ERR_NOT_VALID;
    }

    if (hdr->entry_size < GPT_ENTRY_MIN_SIZE || !ispow2(hdr->entry_size) || hdr->num_entries == 0) {
        LTRACEF("bad entry geometry %u x %u\n", hdr->num_entries, hdr->entry_size);
        return ERR_NOT_VALID;
    }
    uint64_t array_bytes = (uint64_t)hdr->num_entries * hdr->entry_size;
    if (array_bytes > GPT_ENTRY_ARRAY_MAX) {
        LTRACEF("entry array too large (%llu bytes)\n", (unsigned long long)array_bytes);
        return ERR_NOT_VALID;
    }

    // the array has to be on the device, clear of LBA 0, this header and the usable area
    uint64_t array_end =
        hdr->entries_lba + ((array_bytes + dev->block_size - 1) >> dev->block_shift);
    if (hdr->entries_lba == 0 || array_end > dev->block_count ||
        (lba >= hdr->entries_lba && lba < array_end) ||
        (array_end > hdr->first_usable_lba && hdr->entries_lba <= hdr->last_usable_lba)) {
        LTRACEF("bad entry array location %llu\n", (unsigned long long)hdr->entries_lba);
        return ERR_NOT_VALID;
    }

    return NO_ERROR;
}

// Reads and checks the table whose header is at lba. On success *entries is a heap buffer
// holding hdr->num_entries entries, which the caller frees.
static status_t gpt_load(bdev_t *dev, uint8_t *blk, uint64_t lba, struct gpt_header *hdr,
                         uint8_t **entries) {
    ssize_t err = bio_read_block(dev, blk, (bnum_t)lba, 1);
    if (err < 0) {
        return (status_t)err;
    }
    if (err != (ssize_t)dev->block_size) {
        return ERR_IO;
    }

    status_t status = gpt_parse_header(dev, blk, lba, hdr);
    if (status < 0) {
        return status;
    }

    size_t array_bytes = (size_t)hdr->num_entries * hdr->entry_size;
    size_t read_len = ROUNDUP(array_bytes, dev->block_size);
    uint8_t *buf = memalign(CACHE_LINE, ROUNDUP(read_len, CACHE_LINE));
    if (!buf) {
        return ERR_NO_MEMORY;
    }

    err = bio_read(dev, buf, (off_t)(hdr->entries_lba << dev->block_shift), read_len);
    if (err != (ssize_t)read_len) {
        free(buf);
        return (err < 0) ? (status_t)err : ERR_IO;
    }
    if ((uint32_t)crc32(0, buf, array_bytes) != hdr->entries_crc32) {
        LTRACEF("bad entry array crc for header at lba %llu\n", (unsigned long long)lba);
        free(buf);
        return ERR_CRC_FAIL;
    }

    *entries = buf;
    return NO_ERROR;
}

// Folds the UTF-16LE partition name to printable ASCII for the log.
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

int gpt_publish(bdev_t *dev, const char *device) {
    if (dev->block_count < 3 || dev->block_size < 512) {
        return ERR_NOT_FOUND;
    }

    uint8_t *blk = memalign(CACHE_LINE, ROUNDUP(dev->block_size, CACHE_LINE));
    if (!blk) {
        return ERR_NO_MEMORY;
    }

    // The backup's location comes from the device size, not from a primary that failed.
    const uint64_t header_lbas[] = { 1, dev->block_count - 1 };
    struct gpt_header hdr;
    uint8_t *entries = NULL;
    for (size_t i = 0; i < countof(header_lbas); i++) {
        status_t err = gpt_load(dev, blk, header_lbas[i], &hdr, &entries);
        if (err == NO_ERROR) {
            if (i > 0) {
                dprintf(INFO, "gpt: primary table on %s is invalid, using the backup\n", device);
            }
            break;
        }
        dprintf(INFO, "gpt: no valid table at lba %llu on %s (%d)\n",
                (unsigned long long)header_lbas[i], device, err);
    }
    free(blk);
    if (!entries) {
        return ERR_NOT_FOUND;
    }

    static const uint8_t unused_guid[16];
    int count = 0;
    for (uint32_t i = 0; i < hdr.num_entries; i++) {
        struct gpt_entry entry;
        memcpy(&entry, entries + (size_t)i * hdr.entry_size, sizeof(entry));
        if (memcmp(entry.type_guid, unused_guid, sizeof(unused_guid)) == 0) {
            continue;
        }
        if (i >= PARTITION_MAX_SLOTS) {
            dprintf(INFO, "gpt: ignoring partitions in slot %u and above on %s\n", i, device);
            break;
        }

        uint64_t first = LE64(entry.first_lba);
        uint64_t last = LE64(entry.last_lba);
        if (first > last || first < hdr.first_usable_lba || last > hdr.last_usable_lba) {
            dprintf(INFO, "gpt: slot %u on %s has an invalid range [%llu, %llu]\n", i, device,
                    (unsigned long long)first, (unsigned long long)last);
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

    free(entries);
    return count;
}
