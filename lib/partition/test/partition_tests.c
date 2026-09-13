/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#include "../partition_priv.h"

#include <arch/defines.h>
#include <endian.h>
#include <lib/bio.h>
#include <lib/cksum.h>
#include <lib/partition.h>
#include <lib/unittest.h>
#include <lk/compiler.h>
#include <lk/err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE  512
#define DISK_BLOCKS 128
#define DISK_NAME   "parttest"
// 128 entries of 128 bytes, as the usual tools write
#define NUM_ENTRIES  128
#define ENTRY_BLOCKS 32
#define FIRST_USABLE (2 + ENTRY_BLOCKS)
#define LAST_USABLE  (DISK_BLOCKS - 2 - ENTRY_BLOCKS)

struct test_part {
    uint32_t slot;
    uint64_t first;
    uint64_t last;
};

// linux filesystem data
static const uint8_t test_type_guid[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

static void put_le32(uint8_t *p, uint32_t val) {
    val = LE32(val);
    memcpy(p, &val, sizeof(val));
}

static uint8_t *block(uint8_t *disk, uint64_t lba) {
    return disk + lba * BLOCK_SIZE;
}

static void write_gpt_header(uint8_t *disk, uint64_t lba, uint64_t alternate_lba,
                             uint64_t entries_lba, uint32_t entries_crc) {
    struct gpt_header hdr = {};
    memcpy(hdr.signature, GPT_SIGNATURE, sizeof(hdr.signature));
    hdr.revision = LE32(0x10000);
    hdr.header_size = LE32(GPT_HEADER_MIN_SIZE);
    hdr.my_lba = LE64(lba);
    hdr.alternate_lba = LE64(alternate_lba);
    hdr.first_usable_lba = LE64(FIRST_USABLE);
    hdr.last_usable_lba = LE64(LAST_USABLE);
    hdr.entries_lba = LE64(entries_lba);
    hdr.num_entries = LE32(NUM_ENTRIES);
    hdr.entry_size = LE32(sizeof(struct gpt_entry));
    hdr.entries_crc32 = LE32(entries_crc);

    uint8_t *blk = block(disk, lba);
    memset(blk, 0, BLOCK_SIZE);
    memcpy(blk, &hdr, GPT_HEADER_MIN_SIZE);
    put_le32(blk + offsetof(struct gpt_header, header_crc32),
             (uint32_t)crc32(0, blk, GPT_HEADER_MIN_SIZE));
}

// Writes a protective MBR plus primary and backup tables holding parts.
static void build_gpt(uint8_t *disk, const struct test_part *parts, size_t count) {
    memset(disk, 0, DISK_BLOCKS * BLOCK_SIZE);

    uint8_t *mbr_entry = disk + 446;
    mbr_entry[4] = MBR_TYPE_GPT_PROTECTIVE;
    put_le32(mbr_entry + 8, 1);
    put_le32(mbr_entry + 12, DISK_BLOCKS - 1);
    disk[510] = 0x55;
    disk[511] = 0xaa;

    uint8_t *array = block(disk, 2);
    for (size_t i = 0; i < count; i++) {
        struct gpt_entry entry = {};
        memcpy(entry.type_guid, test_type_guid, sizeof(entry.type_guid));
        entry.unique_guid[0] = (uint8_t)(parts[i].slot + 1);
        entry.first_lba = LE64(parts[i].first);
        entry.last_lba = LE64(parts[i].last);
        entry.name[0] = LE16('t');
        memcpy(array + parts[i].slot * sizeof(entry), &entry, sizeof(entry));
    }
    uint32_t crc = (uint32_t)crc32(0, array, NUM_ENTRIES * sizeof(struct gpt_entry));

    const uint64_t backup_entries = DISK_BLOCKS - 1 - ENTRY_BLOCKS;
    memcpy(block(disk, backup_entries), array, ENTRY_BLOCKS * BLOCK_SIZE);
    write_gpt_header(disk, 1, DISK_BLOCKS - 1, 2, crc);
    write_gpt_header(disk, DISK_BLOCKS - 1, 1, backup_entries, crc);
}

static uint8_t *disk_create(void) {
    uint8_t *disk = memalign(CACHE_LINE, DISK_BLOCKS * BLOCK_SIZE);
    if (disk && create_membdev(DISK_NAME, disk, DISK_BLOCKS * BLOCK_SIZE) < 0) {
        free(disk);
        return NULL;
    }
    return disk;
}

static void disk_destroy(uint8_t *disk) {
    // the subdevices hold references to the disk
    partition_unpublish(DISK_NAME);
    bdev_t *dev = bio_open(DISK_NAME);
    if (dev) {
        bio_close(dev);
        bio_unregister_device(dev);
    }
    free(disk);
}

static bool part_exists(uint32_t slot) {
    char name[32];
    snprintf(name, sizeof(name), DISK_NAME "p%u", slot);
    bdev_t *dev = bio_open(name);
    if (dev) {
        bio_close(dev);
    }
    return dev != NULL;
}

// Checks that the subdevice for part exists, has its size, and writes land at its first block.
static bool check_part(uint8_t *disk, const struct test_part *part) {
    BEGIN_TEST;

    char name[32];
    snprintf(name, sizeof(name), DISK_NAME "p%u", part->slot);
    bdev_t *dev = bio_open(name);
    ASSERT_NONNULL(dev, name);
    EXPECT_EQ((bnum_t)(part->last - part->first + 1), dev->block_count, name);

    static const char marker[] = "partition";
    EXPECT_EQ((ssize_t)sizeof(marker), bio_write(dev, marker, 0, sizeof(marker)), name);
    EXPECT_BYTES_EQ((const uint8_t *)marker, block(disk, part->first), sizeof(marker), name);
    bio_close(dev);

    END_TEST;
}

static const struct test_part basic_parts[] = {
    { 0, FIRST_USABLE, 49 },
    { 1, 50, 69 },
    // above the 16 slots MBR ever uses
    { 20, 70, LAST_USABLE },
};

static bool gpt_basic(void) {
    BEGIN_TEST;

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    build_gpt(disk, basic_parts, countof(basic_parts));

    EXPECT_EQ((int)countof(basic_parts), partition_publish(DISK_NAME, 0), "");
    for (size_t i = 0; i < countof(basic_parts); i++) {
        EXPECT_TRUE(check_part(disk, &basic_parts[i]), "");
    }
    EXPECT_FALSE(part_exists(2), "");

    // a rescan replaces the subdevices rather than adding more
    EXPECT_EQ((int)countof(basic_parts), partition_publish(DISK_NAME, 0), "");
    EXPECT_EQ((int)countof(basic_parts), partition_unpublish(DISK_NAME), "");

    disk_destroy(disk);
    END_TEST;
}

static bool gpt_bad_primary_header(void) {
    BEGIN_TEST;

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    build_gpt(disk, basic_parts, countof(basic_parts));
    // still has its signature, fails the crc
    block(disk, 1)[offsetof(struct gpt_header, first_usable_lba)] ^= 1;

    EXPECT_EQ((int)countof(basic_parts), partition_publish(DISK_NAME, 0), "");
    for (size_t i = 0; i < countof(basic_parts); i++) {
        EXPECT_TRUE(check_part(disk, &basic_parts[i]), "");
    }

    disk_destroy(disk);
    END_TEST;
}

static bool gpt_bad_primary_entries(void) {
    BEGIN_TEST;

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    build_gpt(disk, basic_parts, countof(basic_parts));
    // lands in the unused slot 5, so only the array crc notices
    block(disk, 2)[5 * sizeof(struct gpt_entry)] = 1;

    EXPECT_EQ((int)countof(basic_parts), partition_publish(DISK_NAME, 0), "");
    for (size_t i = 0; i < countof(basic_parts); i++) {
        EXPECT_TRUE(check_part(disk, &basic_parts[i]), "");
    }
    EXPECT_FALSE(part_exists(5), "");

    disk_destroy(disk);
    END_TEST;
}

static bool gpt_no_valid_table(void) {
    BEGIN_TEST;

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    build_gpt(disk, basic_parts, countof(basic_parts));
    block(disk, 1)[0] = 0;
    block(disk, DISK_BLOCKS - 1)[0] = 0;

    // the protective MBR entry alone is not published as a partition
    EXPECT_EQ(0, partition_publish(DISK_NAME, 0), "");
    EXPECT_FALSE(part_exists(0), "");

    disk_destroy(disk);
    END_TEST;
}

static bool gpt_rejects_bad_ranges(void) {
    BEGIN_TEST;

    const struct test_part parts[] = {
        { 0, FIRST_USABLE, 49 },
        // would truncate to [50, 59] as a bnum_t
        { 1, (1ULL << 32) + 50, (1ULL << 32) + 59 },
        { 2, 60, LAST_USABLE + 1 },
        { 3, 70, 65 },
        { 4, 1, 10 },
    };

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    build_gpt(disk, parts, countof(parts));

    EXPECT_EQ(1, partition_publish(DISK_NAME, 0), "");
    EXPECT_TRUE(check_part(disk, &parts[0]), "");
    for (uint32_t slot = 1; slot < countof(parts); slot++) {
        EXPECT_FALSE(part_exists(slot), "");
    }

    disk_destroy(disk);
    END_TEST;
}

static bool mbr_basic(void) {
    BEGIN_TEST;

    uint8_t *disk = disk_create();
    ASSERT_NONNULL(disk, "");
    memset(disk, 0, DISK_BLOCKS * BLOCK_SIZE);

    const struct test_part parts[] = {
        { 0, 1, 63 },
        { 1, 64, DISK_BLOCKS - 1 },
    };
    for (size_t i = 0; i < countof(parts); i++) {
        uint8_t *entry = disk + 446 + 16 * parts[i].slot;
        entry[4] = 0x83;
        put_le32(entry + 8, (uint32_t)parts[i].first);
        put_le32(entry + 12, (uint32_t)(parts[i].last - parts[i].first + 1));
    }
    disk[510] = 0x55;
    disk[511] = 0xaa;

    EXPECT_EQ((int)countof(parts), partition_publish(DISK_NAME, 0), "");
    for (size_t i = 0; i < countof(parts); i++) {
        EXPECT_TRUE(check_part(disk, &parts[i]), "");
    }

    disk_destroy(disk);
    END_TEST;
}

BEGIN_TEST_CASE(partition_tests)
RUN_TEST(gpt_basic)
RUN_TEST(gpt_bad_primary_header)
RUN_TEST(gpt_bad_primary_entries)
RUN_TEST(gpt_no_valid_table)
RUN_TEST(gpt_rejects_bad_ranges)
RUN_TEST(mbr_basic)
END_TEST_CASE(partition_tests)
