/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <assert.h>
#include <lib/bio.h>
#include <lk/compiler.h>
#include <stddef.h>
#include <stdint.h>

__BEGIN_CDECLS

// Partitions are published as <device>p<N>, N being the slot in the table they came from.
// Slots at or above this are not published, and partition_unpublish() looks no further.
#define PARTITION_MAX_SLOTS 128

#define MBR_TYPE_GPT_PROTECTIVE 0xee

// On disk GPT structures. All fields are little endian. They are not packed, so parse them
// by copying out of a block buffer; sizeof(struct gpt_header) may include tail padding.
#define GPT_SIGNATURE       "EFI PART"
#define GPT_HEADER_MIN_SIZE 92
#define GPT_ENTRY_MIN_SIZE  128

struct gpt_header {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32; // over header_size bytes, with this field zeroed
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc32; // over num_entries * entry_size bytes
};
static_assert(offsetof(struct gpt_header, header_crc32) == 16, "");
static_assert(offsetof(struct gpt_header, my_lba) == 24, "");
static_assert(offsetof(struct gpt_header, entries_lba) == 72, "");
static_assert(offsetof(struct gpt_header, entries_crc32) == 88, "");

struct gpt_entry {
    uint8_t type_guid[16]; // all zero for an unused slot
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba; // inclusive
    uint64_t attributes;
    uint16_t name[36]; // UTF-16LE
};
static_assert(sizeof(struct gpt_entry) == GPT_ENTRY_MIN_SIZE, "");

// Publishes the partitions in the GPT on dev as subdevices of device, falling back to the
// backup table in the last block if the primary is invalid. Returns the number published,
// or ERR_NOT_FOUND if neither table is usable.
int gpt_publish(bdev_t *dev, const char *device);

__END_CDECLS
