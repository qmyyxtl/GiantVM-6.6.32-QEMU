/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef GVM_PRIVATE_IO_H
#define GVM_PRIVATE_IO_H
#include "exec/memory.h"

/* Private host buffers, never guest RAM and never a DSM memory slot. */
void gvm_private_io_start(int local, int nodes, char **hosts);
void gvm_private_io_stop(void);
bool gvm_private_io_map(AddressSpace *as, hwaddr addr, hwaddr *len,
                        bool write, void **buffer);
bool gvm_private_io_unmap(void *buffer, hwaddr accessed, bool write);
#endif
