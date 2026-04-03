/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*********************************************************************
 * Header file for the MEM-AP target used by other targets to access *
 * Coresight registers.                                              *
 *                                                                   *
 *********************************************************************/

#ifndef OPENOCD_TARGET_MEM_AP_H
#define OPENOCD_TARGET_MEM_AP_H

#include <target/target.h>
#include "target/target_type.h"
#include "helper/log.h"

#define MEM_AP_COMMON_MAGIC 0x4DE4DA50

enum mem_ap_type {
	MEM_AP_TYPE_INVALID,
  MEM_AP_TYPE_ADIV5,	/* mem_ap_get_type_obj returns a (struct adiv5_ap *) */
#ifdef HAVE_CSWP
	MEM_AP_TYPE_CSWP,		/* mem_ap_get_type_obj returns a (cswp_conn_t *) */
#endif /* HAVE_CSWP */
};

enum mem_ap_bus_type {
	MEM_AP_BUS_TYPE_UNKNOWN,
	MEM_AP_BUS_TYPE_APB,
	MEM_AP_BUS_TYPE_AHB,
	MEM_AP_BUS_TYPE_AXI,
};

#define MEM_AP_ID_INVALID 0xffffffffffffffffULL

struct mem_ap;
struct mem_ap_private_config;

/* Scan a set of command line options represented as a JIM object and
 * attempt to match this to an existing mem_ap target. If none is found
 * then create a new one if possible.
 * You should call mem_ap_find_configure() during options parsing with
 * *ppc = NULL the first time. The function will fill *ppc as it goes.
 * Once done call mem_ap_find() which will reslve the mem_ap if possible,
 * create one otherwise then release *ppc.
 */
int mem_ap_find_configure(struct jim_getopt_info *goi,
														struct mem_ap_private_config **ppc);
struct mem_ap *mem_ap_find(struct target *target,
														struct mem_ap_private_config **ppc,
														enum mem_ap_bus_type bus_type_hint);
int mem_ap_release(struct mem_ap *mem_ap);

/* Queued MEM-AP memory mapped single word transfers. */
int mem_ap_read_u32(struct mem_ap *mem_ap,
										target_addr_t address, uint32_t *value);
int mem_ap_write_u32(struct mem_ap *mem_ap,
										target_addr_t address, uint32_t value);
/* Flush all pending transactions in the queue */
int mem_ap_flush(struct mem_ap *mem_ap);

/* Synchronous MEM-AP memory mapped single word transfers. */
int mem_ap_read_atomic_u32(struct mem_ap *mem_ap,
										target_addr_t address, uint32_t *value);
int mem_ap_write_atomic_u32(struct mem_ap *mem_ap,
										target_addr_t address, uint32_t value);

/* Synchronous MEM-AP memory mapped bus block transfers. */
int mem_ap_read_buf(struct mem_ap *mem_ap,
										uint8_t *buffer, uint32_t size, uint32_t count, target_addr_t address);
int mem_ap_write_buf(struct mem_ap *mem_ap,
										const uint8_t *buffer, uint32_t size, uint32_t count, target_addr_t address);

/* Synchronous, non-incrementing buffer functions for accessing fifos. */
int mem_ap_read_buf_noincr(struct mem_ap *mem_ap,
										uint8_t *buffer, uint32_t size, uint32_t count, target_addr_t address);
int mem_ap_write_buf_noincr(struct mem_ap *mem_ap,
										const uint8_t *buffer, uint32_t size, uint32_t count, target_addr_t address);

/* These functions can be used to directly manipulate the Debug Access Port
 * being used by this MEM-AP.
 */
enum mem_ap_type mem_ap_get_type(struct mem_ap *mem_ap);
void *mem_ap_get_type_obj(struct mem_ap *mem_ap);

/* Use this API after resetting the target to re-attach */
int mem_ap_reconnect(struct mem_ap *mem_ap);

const char *mem_ap_get_name(struct mem_ap *mem_ap);

#endif /* OPENOCD_TARGET_MEM_AP_H */
