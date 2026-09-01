#ifndef __LIB_UEFI_BLOCKIO_PROTOCOL_H_
#define __LIB_UEFI_BLOCKIO_PROTOCOL_H_

#include <lib/bio.h>
#include <uefi/types.h>

EfiStatus open_block_device(EfiHandle handle, const void **intf);

EfiStatus list_block_devices(size_t *num_handles, EfiHandle *buf);

// bio_open wrapper that remembers the device so the reference can be released
// at teardown by close_tracked_bdevs(), or individually by close_tracked_bdev()
// when the app calls CloseProtocol.
// On success *out_dev holds the opened device. Returns EFI_STATUS_NOT_FOUND
// when the device does not exist and EFI_STATUS_OUT_OF_RESOURCES when a tracking
// node cannot be allocated (out of memory), so callers can distinguish the two.
EfiStatus open_tracked_bdev(const char *name, bdev_t **out_dev);
// Release a single tracked reference to the named device, balancing one open
// (used by CloseProtocol). Returns true if a tracked open was found and closed;
// an untracked or duplicate close is a harmless no-op.
bool close_tracked_bdev(const char *name);
void close_tracked_bdevs();

#endif
