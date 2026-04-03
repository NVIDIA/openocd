// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Copyright (C) 2016 by Matthias Welwarsky <matthias.welwarsky@sysgo.com>
 * Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "target.h"
#include "target_type.h"
#include "arm_adi_v5.h"
#include "register.h"

#include <jtag/jtag.h>
#include "target/mem_ap.h"
#include "cswp/cswp.h"

#ifdef CLOCK_MONOTONIC_RAW
# define MEM_AP_PERF_CLOCK CLOCK_MONOTONIC_RAW
#else
# define MEM_AP_PERF_CLOCK CLOCK_MONOTONIC
#endif

struct mem_ap {
	int common_magic;
	char *name;
	void *ap;
	uint64_t ap_num;
	bool init_done;
	enum mem_ap_type type;
	unsigned int use_count;
	uint64_t perfstats;
};

struct mem_ap_obj {
	struct list_head lh;
	struct mem_ap mem_ap;
};

struct mem_ap_private_config {
	uint64_t ap_num;
	enum mem_ap_type type;
	char *name;
	/* DAP config vars */
	struct adiv5_dap *dap;
	uint32_t memaccess_tck;
#ifdef HAVE_CSWP
	/* CSWP config vars */
	struct cswp_conn *cswp;
#endif
};

static LIST_HEAD(all_mem_aps);

static struct mem_ap *lookup_mem_ap_by_name(const char *name)
{
	struct mem_ap_obj *obj = NULL;

	list_for_each_entry(obj, &all_mem_aps, lh) {
		if (strcmp(name, obj->mem_ap.name) == 0) {
			return &(obj->mem_ap);
		}
	}
	return NULL;
}

static struct mem_ap *lookup_mem_ap_by_dap(struct adiv5_dap *dap,
																						uint64_t ap_num)
{
	struct mem_ap_obj *obj = NULL;

	list_for_each_entry(obj, &all_mem_aps, lh) {
		if ((obj->mem_ap.type == MEM_AP_TYPE_ADIV5) &&
				(((struct adiv5_ap *)obj->mem_ap.ap)->dap == dap) &&
				(obj->mem_ap.ap_num == ap_num)) {
			return &(obj->mem_ap);
		}
	}
	return NULL;
}

#ifdef HAVE_CSWP
static struct mem_ap *lookup_mem_ap_by_cswp(struct cswp_conn *cswp,
																						uint64_t ap_num)
{
	struct mem_ap_obj *obj = NULL;

	list_for_each_entry(obj, &all_mem_aps, lh) {
		if ((obj->mem_ap.type == MEM_AP_TYPE_CSWP) &&
				(obj->mem_ap.ap == (void *)cswp) &&
				(obj->mem_ap.ap_num == ap_num)) {
			return &(obj->mem_ap);
		}
	}
	return NULL;
}
#endif /* defined(HAVE_CSWP) */

static const char *bus_type_to_name(enum mem_ap_bus_type bus_type)
{
	switch (bus_type) {
		case MEM_AP_BUS_TYPE_UNKNOWN:
			return "Unknown/Invalid";
		case MEM_AP_BUS_TYPE_APB:
			return "APB";
		case MEM_AP_BUS_TYPE_AHB:
			return "AHB";
		case MEM_AP_BUS_TYPE_AXI:
			return "AXI";
	}
	return "INTERNAL ERROR";
};

static const char *mem_ap_type_to_name(enum mem_ap_type bus_type)
{
	switch (bus_type) {
		case MEM_AP_TYPE_INVALID:
			return "Invalid";
		case MEM_AP_TYPE_ADIV5:
			return "ADIv5";
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			return "CSWP";
#endif /* HAVE_CSWP */
	}
	return "INTERNAL ERROR";
};


/**
 * Find the mem_ap target described in the configuration and initialize it.
 *
 * @param mem_ap The MEM-AP to access.
 *
 * @return ERROR_OK for success; all transactions completed successfully.
 * 	Otherwise a fault code.
 */
static int mem_ap_init(struct mem_ap *mem_ap)
{
	int ret = ERROR_OK;

	/* mem_ap_init can be called many times, by the proper target and anything
	   attaching to it for example. */
	if (mem_ap->init_done)
		return ERROR_OK;

	if (mem_ap->type == MEM_AP_TYPE_ADIV5) {
		ret = adiv5_mem_ap_init((struct adiv5_ap *)mem_ap->ap);
	}

	if (ret == ERROR_OK)
		mem_ap->init_done = true;
	return ret;
}

static struct mem_ap *mem_ap_create(struct mem_ap_private_config *pc)
{
	struct mem_ap_obj *obj;

	if (pc->ap_num == DP_APSEL_INVALID) {
		LOG_ERROR("AP number not specified");
		return NULL;
	}

	obj = calloc(1, sizeof(struct mem_ap_obj));
	if (!obj) {
		LOG_ERROR("Out of memory");
		return NULL;
	}

	obj->mem_ap.type = pc->type;
	obj->mem_ap.ap_num = pc->ap_num;
	obj->mem_ap.common_magic = MEM_AP_COMMON_MAGIC;
	obj->mem_ap.name = pc->name;
	obj->mem_ap.use_count = 1;
	obj->mem_ap.perfstats = ~0ULL;
	/* For ADIv5 mem_ap->ap is set in mem_ap_init() later */
	switch (pc->type) {
		case MEM_AP_TYPE_INVALID:
			LOG_ERROR("Invalid configuration, neither -cswp nor -dap was used");
			free(obj);
			return NULL;
		case MEM_AP_TYPE_ADIV5:
			obj->mem_ap.ap = dap_get_ap(pc->dap, pc->ap_num);
			((struct adiv5_ap *)obj->mem_ap.ap)->memaccess_tck = pc->memaccess_tck;
			break;
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			obj->mem_ap.ap = pc->cswp;
			break;
#endif /* defined(HAVE_CSWP) */
	}
	if (!obj->mem_ap.ap) {
		LOG_ERROR("Cannot get AP for MEM-AP of type %s named %s",
							mem_ap_type_to_name(pc->type), pc->name);
		free(obj);
		return NULL;
	}
	list_add_tail(&(obj->lh), &all_mem_aps);
	return &(obj->mem_ap);
}

int mem_ap_release(struct mem_ap *mem_ap)
{
	bool found = false;
	struct mem_ap_obj *obj, *tmp;

	/* Only destroy the object once we have no user */
	if (--mem_ap->use_count > 0) {
		LOG_DEBUG("MEM-AP %s use count decreased to %d", mem_ap->name,
							mem_ap->use_count);
		return ERROR_OK;
	}
	LOG_DEBUG("Destroying MEM-AP %s", mem_ap->name);
	mem_ap->common_magic = 0;
	if (mem_ap->name != NULL)
		free(mem_ap->name);
	if((mem_ap->type == MEM_AP_TYPE_ADIV5)
			&& (mem_ap->ap != NULL)) {
		dap_put_ap((struct adiv5_ap *)mem_ap->ap);
	}
	list_for_each_entry_safe(obj, tmp, &all_mem_aps, lh) {
		if (&(obj->mem_ap) == mem_ap) {
			list_del(&obj->lh);
			free(obj); /* this free mem_ap too */
			found = true;
			break;
		}
	}
	assert(found); /* Has to be true */
	return ERROR_OK;
}

static int mem_ap_target_create(struct target *target, Jim_Interp *interp)
{
	struct mem_ap *mem_ap;
	struct mem_ap_private_config *pc;

	pc = (struct mem_ap_private_config *)target->private_config;
	if (!pc) {
		LOG_ERROR("Configuration not initialized");
		return ERROR_FAIL;
	}

	/* The name is not valid until now */
	assert(target->cmd_name);
	pc->name = strdup(target->cmd_name);
	mem_ap = mem_ap_create(pc);
	if (!mem_ap) {
		LOG_ERROR("mem_ap_create() failed");
		return ERROR_FAIL;
	}
	target->arch_info = mem_ap;
	if (mem_ap->type == MEM_AP_TYPE_ADIV5) {
		struct adiv5_ap *ap = (struct adiv5_ap *)mem_ap->ap;
		target->tap = ap->dap->tap;
		target->has_tap = true;
	} else {
		target->has_tap = false;
	}

	if (!target->gdb_port_override)
		target->gdb_port_override = strdup("disabled");

	return ERROR_OK;
}

static int mem_ap_init_target(struct command_context *cmd_ctx, struct target *target)
{
	LOG_DEBUG("%s", __func__);
	target->state = TARGET_UNKNOWN;
	target->debug_reason = DBG_REASON_UNDEFINED;
	return ERROR_OK;
}

static void mem_ap_deinit_target(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	/* This free target->arch_info */
	mem_ap_release((struct mem_ap *)target->arch_info);
	target->arch_info = NULL;
	/* Note: pc->name is freed in mem_ap_release assuming it was not duplicated */
	free(target->private_config);
	return;
}

static int mem_ap_arch_state(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int mem_ap_poll(struct target *target)
{
	if (target->state == TARGET_UNKNOWN) {
		target->state = TARGET_RUNNING;
		target->debug_reason = DBG_REASON_NOTHALTED;
	}

	return ERROR_OK;
}

static int mem_ap_halt(struct target *target)
{
	LOG_DEBUG("%s", __func__);
	target->state = TARGET_HALTED;
	target->debug_reason = DBG_REASON_DBGRQ;
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);
	return ERROR_OK;
}

static int mem_ap_resume(struct target *target, int current, target_addr_t address,
		int handle_breakpoints, int debug_execution)
{
	LOG_DEBUG("%s", __func__);
	target->state = TARGET_RUNNING;
	target->debug_reason = DBG_REASON_NOTHALTED;
	return ERROR_OK;
}

static int mem_ap_step(struct target *target, int current, target_addr_t address,
				int handle_breakpoints)
{
	LOG_DEBUG("%s", __func__);
	target->state = TARGET_HALTED;
	target->debug_reason = DBG_REASON_DBGRQ;
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);
	return ERROR_OK;
}

static int mem_ap_assert_reset(struct target *target)
{
	target->state = TARGET_RESET;
	target->debug_reason = DBG_REASON_UNDEFINED;

	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int mem_ap_examine(struct target *target)
{
	struct mem_ap *mem_ap = target->arch_info;

	if (!target_was_examined(target)) {
		int ret = mem_ap_init(mem_ap);
		target_set_examined(target);
		target->state = TARGET_UNKNOWN;
		target->debug_reason = DBG_REASON_UNDEFINED;
		return ret;
	}

	return ERROR_OK;
}

static int mem_ap_deassert_reset(struct target *target)
{
	if (target->reset_halt) {
		target->state = TARGET_HALTED;
		target->debug_reason = DBG_REASON_DBGRQ;
		target_call_event_callbacks(target, TARGET_EVENT_HALTED);
	} else {
		target->state = TARGET_RUNNING;
		target->debug_reason = DBG_REASON_NOTHALTED;
	}

	LOG_DEBUG("%s", __func__);
	return ERROR_OK;
}

static int mem_ap_reg_get(struct reg *reg)
{
	return ERROR_OK;
}

static int mem_ap_reg_set(struct reg *reg, uint8_t *buf)
{
	return ERROR_OK;
}

static struct reg_arch_type mem_ap_reg_arch_type = {
	.get = mem_ap_reg_get,
	.set = mem_ap_reg_set,
};

static const char *mem_ap_get_gdb_arch(struct target *target)
{
	return "arm";
}

/*
 * Dummy ARM register emulation:
 * reg[0..15]:  32 bits, r0~r12, sp, lr, pc
 * reg[16..23]: 96 bits, f0~f7
 * reg[24]:     32 bits, fps
 * reg[25]:     32 bits, cpsr
 *
 * Set 'exist' only to reg[0..15], so initial response to GDB is correct
 */
#define NUM_REGS     26
#define MAX_REG_SIZE 96
#define REG_EXIST(n) (1) /* Newer GCCs expect all of them to exist */
#define REG_SIZE(n)  ((((n) >= 16) && ((n) < 24)) ? 96 : 32)

struct mem_ap_alloc_reg_list {
	/* reg_list must be the first field */
	struct reg *reg_list[NUM_REGS];
	struct reg regs[NUM_REGS];
	uint8_t regs_value[MAX_REG_SIZE / 8];
};

static int mem_ap_get_gdb_reg_list(struct target *target, struct reg **reg_list[],
				int *reg_list_size, enum target_register_class reg_class)
{
	struct mem_ap_alloc_reg_list *mem_ap_alloc = calloc(1, sizeof(struct mem_ap_alloc_reg_list));
	if (!mem_ap_alloc) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}

	*reg_list = mem_ap_alloc->reg_list;
	*reg_list_size = NUM_REGS;
	struct reg *regs = mem_ap_alloc->regs;

	for (int i = 0; i < NUM_REGS; i++) {
		regs[i].number = i;
		regs[i].value = mem_ap_alloc->regs_value;
		regs[i].size = REG_SIZE(i);
		regs[i].exist = REG_EXIST(i);
		regs[i].type = &mem_ap_reg_arch_type;
		(*reg_list)[i] = &regs[i];
	}

	return ERROR_OK;
}

static int mem_ap_read_memory(struct target *target, target_addr_t address,
			       uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct mem_ap *mem_ap = target->arch_info;

	LOG_DEBUG("Reading memory at physical address " TARGET_ADDR_FMT
		  "; size %" PRIu32 "; count %" PRIu32, address, size, count);

	if (count == 0 || !buffer)
		return ERROR_COMMAND_SYNTAX_ERROR;

	return mem_ap_read_buf(mem_ap, buffer, size, count, address);
}

static int mem_ap_write_memory(struct target *target, target_addr_t address,
				uint32_t size, uint32_t count,
				const uint8_t *buffer)
{
	struct mem_ap *mem_ap = target->arch_info;

	LOG_DEBUG("Writing memory at physical address " TARGET_ADDR_FMT
		  "; size %" PRIu32 "; count %" PRIu32, address, size, count);

	if (count == 0 || !buffer)
		return ERROR_COMMAND_SYNTAX_ERROR;

	return mem_ap_write_buf(mem_ap, buffer, size, count, address);
}

enum mem_ap_cfg_param {
	CFG_DAP,
	CFG_CSWP,
	CFG_AP_NUM,
	CFG_MEMACCESS_TCK,
};

static const struct jim_nvp nvp_config_opts[] = {
	{ .name = "-dap",       .value = CFG_DAP },
	{ .name = "-cswp",      .value = CFG_CSWP },
	{ .name = "-ap-num",    .value = CFG_AP_NUM },
	{ .name = "-memaccess", .value = CFG_MEMACCESS_TCK },
	{ .name = NULL, .value = -1 }
};

static int mem_ap_fill_pc(struct jim_getopt_info *goi,
													struct mem_ap_private_config *pc)
{
	struct jim_nvp *n;
	int e;

	e = jim_nvp_name2value_obj(goi->interp, nvp_config_opts,
																		goi->argv[0], &n);
	/* Ignore arguments that are not recognized */
	if (e != JIM_OK)
		return JIM_CONTINUE;

	/* We have found a matching option, remove it from array */
	e = jim_getopt_obj(goi, NULL);
	if (e != JIM_OK)
		return e;

	switch (n->value) {
		case CFG_DAP:
		{
			Jim_Obj *o_t;
			e = jim_getopt_obj(goi, &o_t);
			if (e != JIM_OK)
				return e;
			pc->dap = dap_instance_by_jim_obj(goi->interp, o_t);
			if (!pc->dap) {
				Jim_SetResultString(goi->interp, "DAP name invalid!", -1);
				return JIM_ERR;
			}
			pc->type = MEM_AP_TYPE_ADIV5;
			break;
		}

		case CFG_CSWP:
#ifdef HAVE_CSWP
		{
			/* Get CSWP connection name from command line */
			const char *cswp_conn_name;
			jim_getopt_string(goi, &cswp_conn_name, NULL);
			/* Look up CSWP connection by name */
			pc->cswp = cswp_find_connection(cswp_conn_name);
			if (!pc->cswp) {
				Jim_SetResultFormatted(goi->interp, "CSWP connection '%s' not found",
																cswp_conn_name);
				return JIM_ERR;
			}
			pc->type = MEM_AP_TYPE_CSWP;
			break;
		}
#else
			Jim_SetResultString(goi->interp, "CSWP support was not compiled in", -1);
			return JIM_ERR;
#endif
		case CFG_AP_NUM:
		{
			/* jim_wide is a signed 64 bits int, ap_num is unsigned with max 52 bits */
			jim_wide ap_num;
			e = jim_getopt_wide(goi, &ap_num);
			if (e != JIM_OK)
				return e;
			pc->ap_num = ap_num;
			break;
		}

		case CFG_MEMACCESS_TCK:
			{
				/* jim_wide is a signed 64 bits int, memaccess_tck is unsigned with max 32 bits */
				jim_wide memaccess_tck;

				e = jim_getopt_wide(goi, &memaccess_tck);
				if (e != JIM_OK)
					return e;
				if (pc->type != MEM_AP_TYPE_ADIV5) {
					Jim_SetResultString(goi->interp, "-memaccess_tck is only valid if you use -dap first", -1);
					return JIM_ERR;
				}
				if ((memaccess_tck >= 1LL<<32) || (memaccess_tck<0)) {
					Jim_SetResultString(goi->interp, "unsupported value for memaccess_tck", -1);
					return JIM_ERR;
				}
				pc->memaccess_tck = memaccess_tck;
			}
			break;

		default:
			Jim_SetResultString(goi->interp, "INTERNAL ERROR, option not recognized", -1);
			return JIM_ERR;
	};

	return JIM_OK;
}

/* Warning: this function is called as long as it return JIM_CONTINUE */
static int mem_ap_jim_configure(struct target *target, struct jim_getopt_info *goi)
{
	struct mem_ap_private_config *pc;
	int jim_ret;

	if (target->private_config == NULL) {
		pc = calloc(1, sizeof(struct mem_ap_private_config));
		if (!pc) {
			Jim_SetResultFormatted(goi->interp, "Out of memory");
			return JIM_ERR;
		}
		pc->ap_num = DP_APSEL_INVALID;
		pc->memaccess_tck = ADIV5_MEMACCESS_TCK_DEFAULT;
		target->private_config = pc;
	} else {
		pc = target->private_config;
	}

	jim_ret = mem_ap_fill_pc(goi, pc);
	if (jim_ret == JIM_CONTINUE) {
		/* argv[0] is not for us */
		return JIM_CONTINUE;
	} else if (jim_ret != JIM_OK) {
		Jim_SetResultFormatted(goi->interp, "An error was found parsing options for %s", pc->name);
		return JIM_ERR;
	}

	return JIM_OK;
}

/**
 * -------------------------------------------------------------------------
 * handle_cswp_status_command
 *
 * Show the status of CSWP.
 * -------------------------------------------------------------------------
 */
COMMAND_HANDLER(mem_ap_perfstats_command_handler) {
	struct target *target = get_current_target(CMD_CTX);
	struct mem_ap *mem_ap = target->arch_info;
	int64_t value;

	if (CMD_ARGC != 1) {
		command_print(CMD, "usage: <mem_ap> perfstats { on | off | <min_size> }");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (strcmp(CMD_ARGV[0], "on") == 0) {
		value = 0ULL;
		command_print(CMD, "perfstat enabled for %s on all transfers.",
									target_name(target));
	} else if (strcmp(CMD_ARGV[0], "off") == 0) {
		command_print(CMD, "perfstat disabled for %s.", target_name(target));
		value = ~0ULL;
	} else {
		char *p;

		value = strtoll(CMD_ARGV[0], &p, 0);
		if (*p != '\0') {
			command_print(CMD, "Invalid parameter '%s'.", CMD_ARGV[0]);
			return ERROR_FAIL;
		}
		if (value < 0) {
			command_print(CMD, "Value %" PRId64 " is negative, this is not supported.",
										value);
			return ERROR_FAIL;
		}
		command_print(CMD, "perfstat enabled for %s on packets of size %" PRId64
												"B or bigger.", target_name(target), value);
	}
	mem_ap->perfstats = value;
	return ERROR_OK;
}

static const struct command_registration mem_ap_command_handlers[] = {
	{
		.name = "perfstats",
		.handler = mem_ap_perfstats_command_handler,
		.mode = COMMAND_EXEC,
		.help = "Turn on/off the perf statistics feature which display some"
						" performance metrics at the end of each transfer. You can pass"
						" a number too in which case only transfers above that size (in"
						" Bytes) will be instrumented.",
		.usage = "{ on | off | <min_size> }",
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type mem_ap_target = {
	.name = "mem_ap",

	.target_create = mem_ap_target_create,
	.init_target = mem_ap_init_target,
	.deinit_target = mem_ap_deinit_target,
	.examine = mem_ap_examine,
	.target_jim_configure = mem_ap_jim_configure,

	.poll = mem_ap_poll,
	.arch_state = mem_ap_arch_state,

	.halt = mem_ap_halt,
	.resume = mem_ap_resume,
	.step = mem_ap_step,

	.assert_reset = mem_ap_assert_reset,
	.deassert_reset = mem_ap_deassert_reset,

	.get_gdb_arch = mem_ap_get_gdb_arch,
	.get_gdb_reg_list = mem_ap_get_gdb_reg_list,

	.read_memory = mem_ap_read_memory,
	.write_memory = mem_ap_write_memory,

	.commands = mem_ap_command_handlers,
};

/*
 * Helper functions to consolidate functions kept as is for compatibility.
 */
static int mem_ap_read_u32_common(struct mem_ap *mem_ap, target_addr_t address,
																	uint32_t *value, bool async)
{
	int ret;

	switch (mem_ap->type) {
		case MEM_AP_TYPE_ADIV5:
			ret = adiv5_mem_ap_read_u32((struct adiv5_ap *)mem_ap->ap, address,
																		value, async? false : true);
			break;
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			/* CSWP accesses are always synchronous */
			ret = cswp_mem_ap_read((struct cswp_conn *)mem_ap->ap, mem_ap->ap_num,
															address, 4, 1, (uint8_t *)value, true);
			break;
#endif
		default:
			LOG_ERROR("%s() does not support mem_ap type %s", __func__,
									mem_ap_type_to_name(mem_ap->type));
			ret = ERROR_FAIL;
	}
	return ret;
}

static int mem_ap_write_u32_common(struct mem_ap *mem_ap, target_addr_t address,
																	uint32_t value, bool async)
{
	int ret;

	switch (mem_ap->type) {
		case MEM_AP_TYPE_ADIV5:
			ret = adiv5_mem_ap_write_u32((struct adiv5_ap *)mem_ap->ap, address,
																		value, async? false : true);
			break;
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			/* CSWP accesses are always synchronous */
			ret = cswp_mem_ap_write((struct cswp_conn *)mem_ap->ap, mem_ap->ap_num,
															address, 4, 1, (uint8_t *)&value, true);
			break;
#endif
		default:
			LOG_ERROR("%s() does not support mem_ap type %s", __func__,
									mem_ap_type_to_name(mem_ap->type));
			ret = ERROR_FAIL;
	}
	return ret;
}

static int mem_ap_read_buf_common(struct mem_ap *mem_ap, uint8_t *buffer,
																	uint32_t size, uint32_t count,
																	target_addr_t address, bool incr)
{
	int ret;
	struct timespec start_time;

	if (mem_ap->perfstats <= (size*count)) {
		clock_gettime(MEM_AP_PERF_CLOCK, &start_time);
	}

	switch (mem_ap->type) {
		case MEM_AP_TYPE_ADIV5:
			ret = adiv5_mem_ap_read((struct adiv5_ap *)mem_ap->ap, buffer, size,
															count, address, incr);
			break;
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			ret = cswp_mem_ap_read((struct cswp_conn *)mem_ap->ap, mem_ap->ap_num,
															address, size, count, buffer, incr);
			break;
#endif
		default:
			LOG_ERROR("%s() does not support mem_ap type %s", __func__,
									mem_ap_type_to_name(mem_ap->type));
			ret = ERROR_FAIL;
	}

	if (mem_ap->perfstats <= (size*count)) {
		struct timespec end_time;

		clock_gettime(MEM_AP_PERF_CLOCK, &end_time);
    uint64_t elapsed_nsec = (end_time.tv_nsec - start_time.tv_nsec) +
													1000000000 * (end_time.tv_sec - start_time.tv_sec);
    double rate = (double) (size*count) /
        ((double)elapsed_nsec / 1000000000.0);
		LOG_INFO("Transferred %" PRIu32 "B in %" PRIu64 "us (%.0fBps)", size*count,
							elapsed_nsec/1000, rate);
	}

	return ret;
}

static int mem_ap_write_buf_common(struct mem_ap *mem_ap, const uint8_t *buffer,
																	uint32_t size, uint32_t count,
																	target_addr_t address, bool incr)
{
	int ret;
	struct timespec start_time;

	if (mem_ap->perfstats <= (size*count)) {
		clock_gettime(MEM_AP_PERF_CLOCK, &start_time);
	}

	switch (mem_ap->type) {
		case MEM_AP_TYPE_ADIV5:
			ret = adiv5_mem_ap_write((struct adiv5_ap *)mem_ap->ap, buffer, size,
																count, address, incr);
			break;
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			ret = cswp_mem_ap_write((struct cswp_conn *)mem_ap->ap, mem_ap->ap_num,
															address, size, count, buffer, incr);
			break;
#endif
		default:
			LOG_ERROR("%s() does not support mem_ap type %s", __func__,
									mem_ap_type_to_name(mem_ap->type));
			ret = ERROR_FAIL;
	}

	if (mem_ap->perfstats <= (size*count)) {
		struct timespec end_time;

		clock_gettime(MEM_AP_PERF_CLOCK, &end_time);
    uint64_t elapsed_nsec = (end_time.tv_nsec - start_time.tv_nsec) +
													1000000000 * (end_time.tv_sec - start_time.tv_sec);
    double rate = (double) (size*count) /
        ((double)elapsed_nsec / 1000000000.0);
		LOG_INFO("Transferred %" PRIu32 "B in %" PRIu64 "us (%.0fBps)", size*count,
							elapsed_nsec/1000, rate);
	}

	return ret;
}



/*
 * Public APIs to access registers and memory through a MEM-AP. Please
 * use these rather than going directly through ADIv5 APIs.
 */

/**
 * Asynchronous (queued) read of a word from memory or a system register.
 *
 * @param mem_ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the word will be stored when the
 *	transaction queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_read_u32(struct mem_ap *mem_ap, target_addr_t address,
		uint32_t *value)
{
	return mem_ap_read_u32_common(mem_ap, address, value, true);
}

/**
 * Synchronous read of a word from memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param mem_ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the result will be stored.
 *
 * @return ERROR_OK for success; *value holds the result.
 * Otherwise a fault code.
 */
int mem_ap_read_atomic_u32(struct mem_ap *mem_ap, target_addr_t address,
		uint32_t *value)
{
	return mem_ap_read_u32_common(mem_ap, address, value, false);
}

/**
 * Asynchronous (queued) write of a word to memory or a system register.
 *
 * @param mem_ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written to the address when transaction
 *	queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_write_u32(struct mem_ap *mem_ap, target_addr_t address,
		uint32_t value)
{
	return mem_ap_write_u32_common(mem_ap, address, value, true);
}

/**
 * Synchronous write of a word to memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param mem_ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written.
 *
 * @return ERROR_OK for success; the data was written.  Otherwise a fault code.
 */
int mem_ap_write_atomic_u32(struct mem_ap *mem_ap, target_addr_t address,
		uint32_t value)
{
	return mem_ap_write_u32_common(mem_ap, address, value, false);
}

/**
 * Make sure all asynchronous transaction queued before this point are
 * completed.
 *
 * @param mem_ap The MEM-AP to access.
 *
 * @return ERROR_OK for success; all transactions completed successfully.
 * 	Otherwise a fault code.
 */
int mem_ap_flush(struct mem_ap *mem_ap)
{
	int ret = ERROR_FAIL;

	switch (mem_ap->type) {
		case MEM_AP_TYPE_INVALID:
			/* should never happen */
			assert(false);
		case MEM_AP_TYPE_ADIV5:
		{
			struct adiv5_ap *ap = (struct adiv5_ap *)mem_ap->ap;
			/* It does happen during deinit of a non-examined target */
			assert (ap->dap != NULL);
			if (ap->dap->ops != NULL)
				ret = dap_run(ap->dap);
			break;
		}
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			/* do nothing */
			ret = ERROR_OK;
			break;
#endif /* defined(HAVE_CSWP) */
	}
	return ret;
}

int mem_ap_read_buf(struct mem_ap *mem_ap, uint8_t *buffer, uint32_t size,
										uint32_t count, target_addr_t address)
{
	return mem_ap_read_buf_common(mem_ap, buffer, size, count, address, true);
}

int mem_ap_write_buf(struct mem_ap *mem_ap,	const uint8_t *buffer,
											uint32_t size, uint32_t count, target_addr_t address)
{
	return mem_ap_write_buf_common(mem_ap, buffer, size, count, address, true);
}

int mem_ap_read_buf_noincr(struct mem_ap *mem_ap,	uint8_t *buffer,
														uint32_t size, uint32_t count, target_addr_t address)
{
	return mem_ap_read_buf_common(mem_ap, buffer, size, count, address, false);
}

int mem_ap_write_buf_noincr(struct mem_ap *mem_ap, const uint8_t *buffer,
														uint32_t size, uint32_t count, target_addr_t address)
{
	return mem_ap_write_buf_common(mem_ap, buffer, size, count, address, false);
}

/* One can either use -memap <name> or -dap <name> -ap-num <ap>
 * to configure a target that uses mem_ap_find.
 */

enum mem_ap_user_cfg_param {
	MEMAPFIND_CFG_MEMAP,
	MEMAPFIND_CFG_NONE,
};

static const struct jim_nvp nvp_user_config_opts[] = {
	{ .name = "-mem-ap",    .value = MEMAPFIND_CFG_MEMAP },
	{ .name = NULL, 				.value = MEMAPFIND_CFG_NONE }
};

/* Very messed up way of doing things dictated by the way options
 * are parsed in targets, which is one argument at a time.
 * You must call mem_ap_find_configure iteratively until the last
 * argument has been parsed at which point you must call mem_ap_find.
 * ppc must be a pointer to a NULL value at first, it will be filled as we go
 * and be released in mem_ap_find.
 */
int mem_ap_find_configure(struct jim_getopt_info *goi,
														struct mem_ap_private_config **ppc)
{
	int jim_ret;
	struct jim_nvp *n;

	assert (ppc != NULL);
	if (*ppc == NULL) {
		*ppc = (struct mem_ap_private_config *)calloc(1,
																				sizeof(struct mem_ap_private_config));
		(*ppc)->type = MEM_AP_TYPE_INVALID;
		(*ppc)->ap_num = DP_APSEL_INVALID;
		(*ppc)->memaccess_tck = ADIV5_MEMACCESS_TCK_DEFAULT;
	}
	jim_ret = jim_nvp_name2value_obj(goi->interp, nvp_user_config_opts,
																		goi->argv[0], &n);
	if (jim_ret != JIM_OK) {
		return mem_ap_fill_pc(goi, *ppc);
	}

	jim_getopt_obj(goi, NULL);

	if (n->value == MEMAPFIND_CFG_MEMAP) {
		Jim_Obj *o_t;

		jim_ret = jim_getopt_obj(goi, &o_t);
		if (jim_ret != JIM_OK) {
			Jim_SetResultFormatted(goi->interp, "parsing error when analyzing argument of -mem-ap");
			return JIM_ERR;
		}
		(*ppc)->name = strdup(Jim_String(o_t));
		return JIM_OK;
	}
	/* If we reach here someone added to nvp_user_config_opts but did not add
	 * support in this function.
	 */
	Jim_SetResultFormatted(goi->interp, "INTERNAL ERROR: %s is missing code",
													__func__);
	return JIM_ERR;
}

struct mem_ap *mem_ap_find(struct target *target,
														struct mem_ap_private_config **ppc,
														enum mem_ap_bus_type bus_type_hint)
{
	struct mem_ap *mem_ap = NULL;

	assert (ppc && *ppc);
	/* First look for a -memap and if found the corresponding MEM-AP object */
	if ((*ppc)->name != NULL) {
		mem_ap = lookup_mem_ap_by_name((*ppc)->name);
		if (mem_ap == NULL) {
			LOG_ERROR("no mem-ap named '%s' found", (*ppc)->name);
		} else {
			mem_ap->use_count++;
		}
	} else if ((*ppc)->type == MEM_AP_TYPE_ADIV5) {
		/* ADIv5 is a special case due to backward compatibility, we try much
		 * harder rather than expect user to specify -mem-ap.
		 */
		struct adiv5_ap *ap;

		/* If ap_num is not set use the hint to find the right ap */
		if ((*ppc)->ap_num == DP_APSEL_INVALID) {
			enum ap_type adiv5_type = AP_TYPE_APB_AP; /* Set value to appease
																										compiler warning */
			int retval;
			bool search_done = false;

			switch (bus_type_hint) {
				case MEM_AP_BUS_TYPE_UNKNOWN:
					LOG_ERROR("this target requires you to set -ap-num");
					search_done = true;
					break;
				case MEM_AP_BUS_TYPE_APB:
					adiv5_type = AP_TYPE_APB4_AP;
					break;
				case MEM_AP_BUS_TYPE_AHB:
					adiv5_type = AP_TYPE_AHB5H_AP;
					break;
				case MEM_AP_BUS_TYPE_AXI:
					adiv5_type = AP_TYPE_AXI5_AP;
					break;
			}
			while (!search_done) {
				retval = dap_find_get_ap((*ppc)->dap, adiv5_type, &ap);
				if ((retval == ERROR_OK) && (ap != NULL))
					break;
				/* Some bus types have several valid AP profiles */
				switch (adiv5_type) {
					case AP_TYPE_APB4_AP:
						adiv5_type = AP_TYPE_APB_AP;
						break;
					case AP_TYPE_AHB5H_AP:
						adiv5_type = AP_TYPE_AHB5_AP;
						break;
					case AP_TYPE_AHB5_AP:
						adiv5_type = AP_TYPE_AHB3_AP;
						break;
					case AP_TYPE_AXI5_AP:
						adiv5_type = AP_TYPE_AXI_AP;
						break;
					default:
						LOG_ERROR("unable to find an %s MEM-AP on %s",
											bus_type_to_name(bus_type_hint),
											adiv5_dap_name((*ppc)->dap));
						search_done = true;
				}
			}
			if (ap != NULL) {
				(*ppc)->ap_num = ap->ap_num;
			}
		}
		if ((*ppc)->ap_num != DP_APSEL_INVALID) {
			/* Check if a MEM-AP as described already exist */
			mem_ap = lookup_mem_ap_by_dap((*ppc)->dap, (*ppc)->ap_num);
			if (mem_ap == NULL) {
				/* Create a new MEM-AP based on options */
				int n = snprintf(NULL, 0, "%s:%" PRIu64, adiv5_dap_name((*ppc)->dap),
													(*ppc)->ap_num);
				(*ppc)->name = malloc(n+1);
				assert((*ppc)->name != NULL);
				sprintf((*ppc)->name, "%s:%" PRIu64, adiv5_dap_name((*ppc)->dap),
								(*ppc)->ap_num);
				mem_ap = mem_ap_create(*ppc);
			} else {
				mem_ap->use_count++;
			}
		}
#ifdef HAVE_CSWP
	} else if ((*ppc)->type == MEM_AP_TYPE_CSWP) {
		/* Should always be true if MEM_AP_TYPE_CSWP is set */
		assert((*ppc)->cswp != NULL);
		if ((*ppc)->ap_num != DP_APSEL_INVALID) {
			/* Check if a MEM-AP as described already exist */
			mem_ap = lookup_mem_ap_by_cswp((*ppc)->cswp, (*ppc)->ap_num);
			if (mem_ap == NULL) {
				/* Create a new MEM-AP based on options */
				int n = snprintf(NULL, 0, "%s:%" PRIu64, cswp_conn_name((*ppc)->cswp),
													(*ppc)->ap_num);
				(*ppc)->name = malloc(n+1);
				assert((*ppc)->name != NULL);
				sprintf((*ppc)->name, "%s:%" PRIu64, cswp_conn_name((*ppc)->cswp),
								(*ppc)->ap_num);
				mem_ap = mem_ap_create(*ppc);
			} else {
				mem_ap->use_count++;
			}
		}
#endif /* defined(HAVE_CSWP) */
	} else {
		LOG_ERROR("You must specify a MEM-AP with -mem-ap or a DAP with -dap");
	}
	/* Configure the target */
	if (mem_ap != NULL) {
		if (mem_ap->type == MEM_AP_TYPE_ADIV5) {
			struct adiv5_ap *ap = (struct adiv5_ap *)mem_ap->ap;
			target->tap = ap->dap->tap;
			target->has_tap = true;
		} else {
			target->has_tap = false;
		}
	}
	if ((*ppc)->name != NULL)
		free((*ppc)->name);
	free(*ppc);
	return mem_ap;
}

enum mem_ap_type mem_ap_get_type(struct mem_ap *mem_ap)
{
	return mem_ap->type;
}

void *mem_ap_get_type_obj(struct mem_ap *mem_ap)
{
	return mem_ap->ap;
}

int mem_ap_reconnect(struct mem_ap *mem_ap)
{
	int ret = ERROR_FAIL;

	switch (mem_ap->type) {
		case MEM_AP_TYPE_INVALID:
			/* Should never happen */
			assert(false);
		case MEM_AP_TYPE_ADIV5:
		{
			struct adiv5_ap *ap = (struct adiv5_ap *)mem_ap->ap;
			/* Only the DAP need to be reconfigured */
			ret = dap_dp_init_or_reconnect(ap->dap);
			break;
		}
#ifdef HAVE_CSWP
		case MEM_AP_TYPE_CSWP:
			LOG_ERROR("Reinit not supported yet with CSWP");
			ret = ERROR_FAIL;
			break;
#endif /* define(HAVE_CSWP) */
	}
	return ret;
}

const char *mem_ap_get_name(struct mem_ap *mem_ap)
{
	return mem_ap->name;
}
