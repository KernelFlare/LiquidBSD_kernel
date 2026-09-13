/*
 * Copyright (c) 2009 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
#pragma once

#include <lk/compiler.h>
#include <sys/types.h>

__BEGIN_CDECLS

/* examine and try to publish partitions on a particular device at a particular offset */
int partition_publish(const char *device, off_t offset);

/* print the partition tables found on a device and what publishing would do with them,
 * without publishing anything */
int partition_dump(const char *device, off_t offset);

/* remove any published subdevices on this device */
int partition_unpublish(const char *device);

__END_CDECLS
