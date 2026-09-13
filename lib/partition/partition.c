/*
 * Copyright (c) 2009 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "lib/partition.h"

#include <lk/debug.h>
#include <lk/err.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <lk/compiler.h>
#include <stdlib.h>
#include <arch.h>
#include <endian.h>
#include <lib/bio.h>
#include <assert.h>

#include "partition_priv.h"

struct chs {
    uint8_t c;
    uint8_t h;
    uint8_t s;
};

struct mbr_part {
    uint8_t status;
    struct chs start;
    uint8_t type;
    struct chs end;
    uint32_t lba_start;
    uint32_t lba_length;
};
static_assert(sizeof(struct mbr_part) == 16, "");

/* an MBR with its fields in host order */
struct mbr {
    uint32_t disk_signature;
    struct mbr_part part[4];
    bool protective; /* one of the entries covers a GPT */
};

void partition_format_size(char *buf, size_t len, uint64_t bytes) {
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    size_t unit = 0;
    uint64_t rem = 0;
    while (bytes >= 1024 && unit + 1 < countof(units)) {
        rem = bytes & 1023;
        bytes >>= 10;
        unit++;
    }
    if (unit == 0) {
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(buf, len, "%llu.%llu %s", (unsigned long long)bytes,
                 (unsigned long long)(rem * 10 / 1024), units[unit]);
    }
}

/* Reads the MBR at offset. Returns 1 if it has a boot signature, 0 if not. */
static int mbr_read(bdev_t *dev, off_t offset, struct mbr *mbr) {
    // get a dma aligned and padded block to read info
    STACKBUF_DMA_ALIGN(buf, dev->block_size);

    ssize_t err = bio_read(dev, buf, offset, 512);
    if (err < 0)
        return (int)err;

    /* look for the aa55 tag */
    if (buf[510] != 0x55 || buf[511] != 0xaa)
        return 0;

    memcpy(&mbr->disk_signature, buf + 440, sizeof(mbr->disk_signature));
    mbr->disk_signature = LE32(mbr->disk_signature);
    memcpy(mbr->part, buf + 446, sizeof(mbr->part));

    mbr->protective = false;
    for (int i = 0; i < 4; i++) {
        mbr->part[i].lba_start = LE32(mbr->part[i].lba_start);
        mbr->part[i].lba_length = LE32(mbr->part[i].lba_length);
        if (mbr->part[i].type == MBR_TYPE_GPT_PROTECTIVE)
            mbr->protective = true;
    }

    return 1;
}

/* Returns NULL if the entry can be published, otherwise why not. */
static const char *mbr_partition_problem(bdev_t *dev, const struct mbr_part *part) {
    if (part->type == 0)
        return "empty";
    if (part->type == MBR_TYPE_GPT_PROTECTIVE)
        return "GPT protective entry";
    if (part->status != 0x80 && part->status != 0x00)
        return "bad status byte";

    /* make sure the range fits within the device */
    if (part->lba_start >= dev->block_count)
        return "starts past the end of the device";
    if ((uint64_t)part->lba_start + part->lba_length > dev->block_count)
        return "extends past the end of the device";

    /* that's about all we can do, MBR has no other good way to see if it's valid */

    return NULL;
}

int partition_publish(const char *device, off_t offset) {
    int err = 0;
    int count = 0;

    // clear any partitions that may have already existed
    partition_unpublish(device);

    bdev_t *dev = bio_open(device);
    if (!dev) {
        printf("partition_publish: unable to open device\n");
        return -1;
    }

    struct mbr mbr;
    err = mbr_read(dev, offset, &mbr);
    if (err > 0) {
        err = 0;

#if LK_DEBUGLEVEL >= INFO
        dprintf(INFO, "mbr partition table dump:\n");
        for (int i = 0; i < 4; i++) {
            const struct mbr_part *part = &mbr.part[i];
            dprintf(INFO, "\t%i: status 0x%hhx, type 0x%hhx, start 0x%x, len 0x%x\n", i,
                    part->status, part->type, part->lba_start, part->lba_length);
        }
#endif

        /* A GPT is only used behind a protective entry, as the UEFI spec requires, so a stale
         * GPT left on a disk since repartitioned with an MBR is ignored. GPT LBAs are
         * absolute, so only a table at the start of the device can be one. Without a usable
         * GPT, any other entries in the MBR are published. */
        int gpt_count = ERR_NOT_FOUND;
        if (mbr.protective && offset == 0)
            gpt_count = gpt_publish(dev, device);

        if (gpt_count >= 0) {
            count = gpt_count;
        } else {
            for (int i = 0; i < 4; i++) {
                if (mbr_partition_problem(dev, &mbr.part[i]))
                    continue;

                char subdevice[128];
                snprintf(subdevice, sizeof(subdevice), "%sp%d", device, i);

                status_t perr = bio_publish_subdevice(device, subdevice, mbr.part[i].lba_start,
                                                      mbr.part[i].lba_length);
                if (perr < 0) {
                    dprintf(INFO, "error publishing subdevice '%s'\n", subdevice);
                    continue;
                }
                count++;
            }
        }
    }

    bio_close(dev);

    if (err >= 0) {
        dprintf(INFO, "partition_publish: %u partition%s found\n", count, (count == 1) ? "" : "s");
    }

    return (err < 0) ? err : count;
}

int partition_dump(const char *device, off_t offset) {
    bdev_t *dev = bio_open(device);
    if (!dev) {
        printf("partition_dump: unable to open device '%s'\n", device);
        return ERR_NOT_FOUND;
    }

    char size[16];
    partition_format_size(size, sizeof(size), (uint64_t)dev->total_size);
    printf("%s: %u blocks of %zu bytes (%s)\n", device, dev->block_count, dev->block_size, size);

    struct mbr mbr;
    int err = mbr_read(dev, offset, &mbr);
    if (err < 0) {
        printf("MBR: read error %d\n", err);
    } else if (err == 0) {
        printf("MBR: no boot signature at offset %lld\n", (long long)offset);
    } else {
        printf("MBR at offset %lld: disk signature 0x%08x\n", (long long)offset,
               mbr.disk_signature);
        printf("  %4s %6s %4s %12s %12s %10s\n", "slot", "status", "type", "first lba", "blocks",
               "size");
        for (int i = 0; i < 4; i++) {
            const struct mbr_part *part = &mbr.part[i];
            if (part->type == 0)
                continue;
            partition_format_size(size, sizeof(size), (uint64_t)part->lba_length * dev->block_size);
            const char *problem = mbr_partition_problem(dev, part);
            printf("  %4d   0x%02x 0x%02x %12u %12u %10s%s%s\n", i, part->status, part->type,
                   part->lba_start, part->lba_length, size, problem ? "  not published: " : "",
                   problem ? problem : "");
        }
    }

    /* Shown even without a protective entry, so a table partition_publish() ignores still
     * turns up. */
    if (offset == 0)
        gpt_dump(dev, err > 0 && mbr.protective);

    bio_close(dev);
    return (err < 0) ? err : 0;
}

int partition_unpublish(const char *device) {
    int i;
    int count;
    bdev_t *dev;
    char devname[512];

    count = 0;
    for (i=0; i < PARTITION_MAX_SLOTS; i++) {
        snprintf(devname, sizeof(devname), "%sp%d", device, i);

        dev = bio_open(devname);
        if (!dev)
            continue;

        bio_unregister_device(dev);
        bio_close(dev);
        count++;
    }

    return count;
}
