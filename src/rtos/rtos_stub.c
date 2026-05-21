// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Stub implementation when RTOS support is disabled at configure time  *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef ENABLE_RTOS

#include <string.h>

#include "rtos.h"
#include "target/target.h"
#include "helper/log.h"
#include "helper/jim-nvp.h"
#include "server/gdb_server.h"

int rtos_create(struct jim_getopt_info *goi, struct target *target)
{
	int e;
	const char *cp;

	if (!goi->isconfigure && goi->argc != 0) {
		Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "NO PARAMS");
		return JIM_ERR;
	}

	e = jim_getopt_string(goi, &cp, NULL);
	if (e != JIM_OK)
		return e;

	if (strcmp(cp, "none") == 0) {
		target->rtos = NULL;
		return JIM_OK;
	}

	LOG_ERROR("RTOS support is disabled in this build (configure with --enable-rtos)");
	return JIM_ERR;
}

void rtos_destroy(struct target *target)
{
	if (target)
		target->rtos = NULL;
}

int rtos_set_reg(struct connection *connection, int reg_num, uint8_t *reg_value)
{
	(void)connection;
	(void)reg_num;
	(void)reg_value;
	return ERROR_FAIL;
}

int rtos_generic_stack_read(struct target *target,
		const struct rtos_register_stacking *stacking,
		int64_t stack_ptr,
		struct rtos_reg **reg_list,
		int *num_regs)
{
	(void)target;
	(void)stacking;
	(void)stack_ptr;
	(void)reg_list;
	(void)num_regs;
	return ERROR_FAIL;
}

int gdb_thread_packet(struct connection *connection, char const *packet, int packet_size)
{
	(void)connection;
	(void)packet;
	(void)packet_size;
	return GDB_THREAD_PACKET_NOT_CONSUMED;
}

int rtos_get_gdb_reg(struct connection *connection, int reg_num)
{
	(void)connection;
	(void)reg_num;
	return ERROR_FAIL;
}

int rtos_get_gdb_reg_list(struct connection *connection)
{
	(void)connection;
	return ERROR_FAIL;
}

int rtos_update_threads(struct target *target)
{
	(void)target;
	return ERROR_OK;
}

void rtos_free_threadlist(struct rtos *rtos)
{
	(void)rtos;
}

int rtos_smp_init(struct target *target)
{
	(void)target;
	return ERROR_OK;
}

int rtos_qsymbol(struct connection *connection, char const *packet, int packet_size)
{
	(void)connection;
	(void)packet;
	(void)packet_size;
	return ERROR_FAIL;
}

int rtos_read_buffer(struct target *target, target_addr_t address,
		uint32_t size, uint8_t *buffer)
{
	(void)target;
	(void)address;
	(void)size;
	(void)buffer;
	return ERROR_FAIL;
}

int rtos_write_buffer(struct target *target, target_addr_t address,
		uint32_t size, const uint8_t *buffer)
{
	(void)target;
	(void)address;
	(void)size;
	(void)buffer;
	return ERROR_FAIL;
}

#endif /* !ENABLE_RTOS */
