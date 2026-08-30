#ifndef __LIB_UEFI_BLOCKIO_PROTOCOL_H_
#define __LIB_UEFI_BLOCKIO_PROTOCOL_H_

#include <lib/bio.h>
#include <uefi/types.h>

EfiStatus open_block_device(EfiHandle handle, const void **intf);

EfiStatus list_block_devices(size_t *num_handles, EfiHandle *buf);

// bio_open wrapper that remembers the device so the reference can be
// released by close_tracked_bdevs() when the current image run tears down.
// On success *out_dev holds the opened device. Returns EFI_STATUS_NOT_FOUND
// when the device does not exist and EFI_STATUS_OUT_OF_RESOURCES when the
// tracking table is full (so callers can distinguish the two).
EfiStatus open_tracked_bdev(const char *name, bdev_t **out_dev);
void close_tracked_bdevs();

#endif
