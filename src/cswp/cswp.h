// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef OPENOCD_CSWP_CSWP_H
#define OPENOCD_CSWP_CSWP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <dlfcn.h>

/* Forward declarations */
struct command_context;

/**
 * ----------------------------------------------------------------------------
 * @file cswp.h
 * @brief Header file for CoreSight Wire Protocol (CSWP) client integration for
 * OpenOCD
 * ----------------------------------------------------------------------------
 */

struct cswp_conn;

/* CSWP connection management */
struct cswp_conn*	cswp_find_connection(const char *name);
const char *			cswp_conn_name(struct cswp_conn* conn);
bool 							transport_is_cswp(void);

/* CSWP memory access functions */
int cswp_mem_ap_read(struct cswp_conn* conn, uint64_t devID, uint64_t address,
											uint32_t acc_bytes, uint32_t num_accesses, uint8_t* buf,
											bool incr);
int cswp_mem_ap_write(struct cswp_conn* conn, uint64_t devID, uint64_t address,
											uint32_t acc_bytes, uint32_t num_accesses,
											const uint8_t* buf, bool incr);

#endif /* OPENOCD_CSWP_CSWP_H */
