// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * ----------------------------------------------------------------------------
 * @file cswp.c
 * @brief CoreSight Wire Protocol (CSWP) client integration for OpenOCD
 *
 * This module provides integration between OpenOCD and CSWP (CoreSight Wire
 * Protocol) allowing mem-ap operations to be redirected through a CSWP server.
 * ----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>
#include <helper/command.h>
#include <helper/list.h>
#include <helper/types.h>
#include <helper/binarybuffer.h>
#include <transport/transport.h>
#include "cswp.h"
#include <cswp_types.h>

/**
 * ----------------------------------------------------------------------------
 * CSWP specific constants
 *
 * These defines can change in the future based on the debugger, host or server
 * implementation.
 * ----------------------------------------------------------------------------
 */
/* CSWP client constants */
#define MAX_NUM_DEVICES			 10U
#define MAX_DEV_NAME_LEN		 50U
#define MAX_DEV_TYPE_LEN		 30U
#define MAX_DEV_INFO_LEN		 50U
#define MAX_SERVER_NAME_LEN	50U
#define CLIENT_PROTOCOL_V		0x100U
#define CLIENT_ID_STR				"CSWP_OPENOCD"

/* CSWP access size constants */
#define CSWP_ACCESS_SIZE_DEF 0
#define CSWP_ACCESS_SIZE_8	 1
#define CSWP_ACCESS_SIZE_16	2
#define CSWP_ACCESS_SIZE_32	3
#define CSWP_ACCESS_SIZE_64	4

/* CSWP device capabilities */
#define CSWP_CAP_REG			 0x00000001 /* Device supports register read/write. */
#define CSWP_CAP_MEM			 0x00000002 /* Device supports memory read/write. */
#define CSWP_CAP_MEM_POLL	0x00000200 /* Device supports memory polling. */

/* Memory read/write request constants */
#define MEM_MAX_RW_SZ					16352 /* Max size of a single read/write request
																				i.e (CSWP_REQ_RSP_BUFFER_SIZE - 32)
																				bytes. */
#define DEV0_MEM_MAX_BIN_SZ		 4096 /* Max dump/fill size in bytes enforced by
																				CSWP to avoid RAS errors. */
#define DEV1_MEM_MAX_BIN_SZ	 262144 /* Max dump/fill size in bytes enforced by
																				CSWP from usability perspective. */
#define MEM_MAX_BIN_SZ				262144 /* Max array size for read/write request
																				data. */
#define MAX_NUM_SUBREQ						 4 /* worse case req is (8*n byte)+ 4 byte
																				+ 2 byte + 1 byte = 4 */

/* Structure for subreqs in a large memory read/write request */
typedef struct {
	uint64_t					 saddr;		 // starting address for subreq
	unsigned					 acc_sz;		// access size encoding of subreq
	uint64_t					 size;			// size of subreq
	uint64_t					 buf_idx;	 // rw_values starting index for subreq
} subreq_info;

/* Structure for memory read/write request */
typedef struct {
	uint32_t					devID;
	uint64_t					start_addr;
	uint64_t					total_bytes;
	uint8_t						rw_values[MEM_MAX_BIN_SZ]; // Used to read or write bytes
	uint64_t					num_subreqs;
	subreq_info				subreqs[MAX_NUM_SUBREQ];
	uint64_t					flags;
} mem_req_info;

/**
 * CSWP device description
 */
typedef struct {
	const char *name;
	const char *type;
	const char *info;
	uint32_t capabilities;
	uint32_t capabilities_data;
} cswp_device_t;

static char *cswp_library_path = NULL;

/**
 * ------------------------------------------------------------------------
 * CSWP connection structure
 *
 * Contains all the state and function pointers needed to communicate
 * with a CSWP server through the dynamically loaded client library.
 * ------------------------------------------------------------------------
 */
typedef struct cswp_conn {

	void *lib_handle;	 /* Handle to the dynamically loaded CSWP library */
	struct {
		char* type;		 /* Connection type */
		char* cfg_str;	/* Connection configuration string */
	} conn_params;

	void *client;			 /* Client context */
	bool	enabled;			/* Flag indicating if CSWP is enabled and ready for use */

	/* Function pointers to CSWP client API functions */
	struct {
		void* (*create_client)(char*, char*);
		void* (*delete_client)(void*);
		int	 (*init)(void*, const char*, uint64_t, uint64_t*, char*, size_t,
									unsigned*);
		int	 (*term)(void*);
		int	 (*device_open)(void*, unsigned, char*, size_t);
		int	 (*device_close)(void*, unsigned);
		int	 (*get_devices)(void*, unsigned*, char**, size_t, size_t, char**,
												size_t, size_t);
		int	 (*get_device_capabilities)(void*, uint64_t, unsigned*, unsigned*);
		int	 (*device_reg_read)(void*, unsigned, size_t, const unsigned*, uint32_t*,
														size_t);
		int	 (*device_reg_write)(void*,	unsigned, size_t, const unsigned*,
															const uint32_t*, size_t);
		int	 (*device_mem_read)(void*, unsigned, uint64_t, size_t, unsigned,
															unsigned, uint8_t*, size_t*);
		int	 (*device_mem_write)(void*, unsigned, uint64_t, size_t, unsigned,
															unsigned, const uint8_t*);
		int	 (*batch_begin)(void*, int);
		int	 (*batch_end)(void*, unsigned*);
		const char *(*decode_error)(void*, int);
	} api;
	size_t num_devices;
	cswp_device_t *devices;
} cswp_conn_t;

/**
 * List of all cswp connections
 */
typedef struct {
		struct list_head list;
		char* name;
		cswp_conn_t* conn;
} cswp_conn_entry_t;

static LIST_HEAD(cswp_connections);


/**
 * -------------------------------------------------------------------------
 * cswp_is_enabled
 *
 * Check if CSWP connection is enabled and active.
 * -------------------------------------------------------------------------
 */
static inline bool
cswp_is_enabled(cswp_conn_t* conn) {
	return ((conn) ? conn->enabled : false);
}

/**
 * -------------------------------------------------------------------------
 * cswp_load_client_lib
 *
 * Loads the CSWP client library and sets up CSWP API pointers.
 * -------------------------------------------------------------------------
 */
static int
cswp_load_client_lib(cswp_conn_t* conn, const char *library_path) {

	if (!conn) {
		LOG_ERROR("CSWP connection structure has not been allocated.");
		return ERROR_FAIL;
	}
	if (cswp_is_enabled(conn)) {
		LOG_ERROR("CSWP already enabled, disable it first before loading a new"
							" library.");
		return ERROR_FAIL;
	}

	conn->lib_handle = dlopen(library_path, RTLD_LAZY);
	if (!conn->lib_handle) {
		LOG_ERROR("Failed to load CSWP library '%s': %s", library_path, dlerror());
		return ERROR_FAIL;
	}

	dlerror(); /* Clear any existing error */

	/* Load function symbols */
	conn->api.create_client 	= dlsym(conn->lib_handle, "cswp_create_c");
	conn->api.delete_client 	= dlsym(conn->lib_handle, "cswp_delete_c");
	conn->api.init						= dlsym(conn->lib_handle, "cswp_init_c");
	conn->api.term						= dlsym(conn->lib_handle, "cswp_term_c");
	conn->api.device_open	 		= dlsym(conn->lib_handle, "cswp_device_open_c");
	conn->api.device_close		= dlsym(conn->lib_handle, "cswp_device_close_c");
	conn->api.get_devices	 		= dlsym(conn->lib_handle, "cswp_get_devices_c");
	conn->api.get_device_capabilities = dlsym(conn->lib_handle,
																							"cswp_get_device_capabilities_c");
	conn->api.device_reg_read = dlsym(conn->lib_handle,"cswp_device_reg_read_c");
	conn->api.device_reg_write= dlsym(conn->lib_handle, "cswp_device_reg_write_c");
	conn->api.device_mem_read = dlsym(conn->lib_handle, "cswp_device_mem_read_c");
	conn->api.device_mem_write= dlsym(conn->lib_handle, "cswp_device_mem_write_c");
	conn->api.batch_begin		 = dlsym(conn->lib_handle, "cswp_batch_begin_c");
	conn->api.batch_end			 = dlsym(conn->lib_handle, "cswp_batch_end_c");
	conn->api.decode_error	 = dlsym(conn->lib_handle, "cswp_decode_error_c");

	/* Verify all symbols were loaded */
	if ((!conn->api.create_client)	 || (!conn->api.delete_client)					 ||
			(!conn->api.init)						|| (!conn->api.term)										||
			(!conn->api.device_open)		 || (!conn->api.device_close)						||
			(!conn->api.get_devices)		 || (!conn->api.get_device_capabilities) ||
			(!conn->api.device_reg_read) || (!conn->api.device_reg_write)				||
			(!conn->api.device_mem_read) || (!conn->api.device_mem_write)				||
			(!conn->api.batch_begin)		 || (!conn->api.batch_end)  						||
			(!conn->api.decode_error)) {
		LOG_ERROR("Failed to find required CSWP symbols from library");
		dlclose(conn->lib_handle);
		conn->lib_handle = NULL;
		return ERROR_FAIL;
	}

	LOG_INFO("CSWP library loaded successfully");
	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * get_dev_info
 *
 * Get the device information for all available devices.
 * -------------------------------------------------------------------------
 */
static size_t
cswp_get_dev_info(cswp_conn_t* conn, char** deviceList, size_t deviceListSize,
									size_t deviceListEntrySize, char** deviceTypes,
									size_t deviceTypeSize, size_t deviceTypeEntrySize) {
	if (!conn) {
		LOG_ERROR("CSWP connection is NULL.");
		return ERROR_FAIL;
	}
	if (!cswp_is_enabled(conn)) {
		LOG_ERROR("CSWP is not enabled.");
		return ERROR_FAIL;
	}

	uint32_t deviceCount;

	int retval = conn->api.get_devices(conn->client, &deviceCount,
																		 deviceList, deviceListSize,
																		 deviceListEntrySize, deviceTypes,
																		 deviceTypeSize, deviceTypeEntrySize);

	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Failed to get device information.");
		return ERROR_FAIL;
	}

	return (size_t)deviceCount;
}

/**
 * -------------------------------------------------------------------------
 * get_dev_cap
 *
 * Get the device capabilities for all available devices.
 * -------------------------------------------------------------------------
 */
static int
cswp_get_dev_cap(cswp_conn_t* conn, uint32_t* capabilities,
									uint32_t* capabilityData, uint32_t num_devices) {
	if (!conn) {
		LOG_ERROR("CSWP connection is NULL.");
		return ERROR_FAIL;
	}
	if (!cswp_is_enabled(conn)) {
		LOG_ERROR("CSWP is not enabled.");
		return ERROR_FAIL;
	}

	uint32_t ops_completed = 0;
	int retval						 = CSWP_SUCCESS;

	/* Start batch for cswp_get_device_capabilities */
	retval = conn->api.batch_begin(conn->client, 1 /*abort on error*/);
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Error [%d] in beginning batch for cswp_get_device_capabilities.",
							retval);
		return ERROR_FAIL;
	}
	/* Create batch for cswp_get_device_capabilities */
	for (uint32_t i = 0; (i < num_devices); i++) {
		retval = conn->api.get_device_capabilities(conn->client, i,
																								&capabilities[i],
																								&capabilityData[i]);
		if (retval != CSWP_SUCCESS) {
			LOG_ERROR("Error [%d] in creating batch for"
								" cswp_get_device_capabilities.", retval);
			return ERROR_FAIL;
		}
	}
	/* Submit batch for cswp_get_device_capabilities */
	retval = conn->api.batch_end(conn->client, &ops_completed);
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Error [%d] Batch submission for cswp_get_device_capabilities"
							" failed.", retval);
		return ERROR_FAIL;
	}

	if (ops_completed != num_devices) {
		LOG_ERROR("Not all device capabilities were retrieved.");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_enable
 *
 * Start the CSWP server, connect to it, and open all available devices.
 * -------------------------------------------------------------------------
 */
static int
cswp_enable(cswp_conn_t* conn) {
	if (!conn) {
		LOG_ERROR("CSWP connection is NULL.");
		return ERROR_FAIL;
	}
	if (cswp_is_enabled(conn)) {
		LOG_ERROR("CSWP connection already enabled, cannot start again.");
		return ERROR_FAIL;
	}

	if (!conn->client) {
		LOG_ERROR("CSWP client context not created");
		return ERROR_FAIL;
	}

	/* Connect to CSWP server and start the server */
	int retval = 0;
	const char	clientID[20]					= CLIENT_ID_STR;
	uint64_t		ProtocolVersion				= CLIENT_PROTOCOL_V;
	size_t			serverIDSize					= MAX_SERVER_NAME_LEN;
	uint64_t		serverProtocolVersion	= 0;
	char				serverID[MAX_SERVER_NAME_LEN];
	uint32_t		serverVersion					= 0;

	retval = conn->api.init(conn->client, clientID, ProtocolVersion,
														&serverProtocolVersion, serverID, serverIDSize,
														&serverVersion);
	if (retval == CSWP_FAILED) {
		// If the interface was not cleanly shut down it might still be open
		// attemp to close it.
		LOG_WARNING("Failed to connect to CSWP server (%d), attempting to close any"
							" previous session and try again...", retval);
		conn->api.term(conn->client);
		retval = conn->api.init(conn->client, clientID, ProtocolVersion,
														&serverProtocolVersion, serverID, serverIDSize,
														&serverVersion);
	}
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Failed to connect and start CSWP server. Error: %s (%d)",
							conn->api.decode_error(conn, retval), retval);
		return ERROR_FAIL;
	}
	LOG_INFO("CSWP server has started. Server Protocol Version is [0x%" PRIx64
						"], Server ID is [%s] and Server Version is [0x%x]",
						serverProtocolVersion, serverID, serverVersion);
	conn->enabled = true; /* Set CSWP debug interface to enabled */

	/* Get device information */
	char deviceList_2D[MAX_NUM_DEVICES][MAX_DEV_NAME_LEN];
	char deviceTypes_2D[MAX_NUM_DEVICES][MAX_DEV_TYPE_LEN];
	char * deviceList[MAX_NUM_DEVICES] = {0};
	char * deviceTypes[MAX_NUM_DEVICES] = {0};
	for (unsigned i = 0; i < MAX_NUM_DEVICES; ++i) {
		deviceList[i]	= deviceList_2D[i];
		deviceTypes[i] = deviceTypes_2D[i];
	}
	size_t deviceListSize				= MAX_NUM_DEVICES;
	size_t deviceListEntrySize	= MAX_DEV_NAME_LEN;
	size_t deviceTypeSize				= MAX_NUM_DEVICES;
	size_t deviceTypeEntrySize	= MAX_DEV_TYPE_LEN;
	retval = cswp_get_dev_info(conn, deviceList, deviceListSize,
															deviceListEntrySize, deviceTypes,	deviceTypeSize,
															deviceTypeEntrySize);
	if (retval < ERROR_OK) {
		LOG_ERROR("Failed to get device information.");
		return retval;
	}
	conn->num_devices = (size_t)retval;

	/* Get device capabilities */
	uint32_t capabilities[conn->num_devices];
	uint32_t capabilityData[conn->num_devices];
	retval = cswp_get_dev_cap(conn, capabilities, capabilityData,
														conn->num_devices);
	if (retval != ERROR_OK) {
		LOG_ERROR("Failed to get device capabilities.");
		return retval;
	}

	/* Open all available devices with batch command */
	char deviceInfoList_2D[conn->num_devices][MAX_DEV_INFO_LEN];
	char *deviceInfoList[conn->num_devices];
	for (unsigned i = 0; i < conn->num_devices; ++i)
		deviceInfoList[i] = deviceInfoList_2D[i];
	size_t	 deviceInfoSize = MAX_DEV_INFO_LEN;
	uint32_t ops_completed	= 0;

	/* Start batch for cswp_device_open */
	retval = conn->api.batch_begin(conn->client, 1 /*abort on error*/);
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Error [%d] in beginning batch for cswp_device_open.", retval);
		return ERROR_FAIL;
	}
	/* Create batch for cswp_device_open */
	for (uint32_t i = 0; (i < conn->num_devices); i++) {
		retval = conn->api.device_open(conn->client, i, deviceInfoList[i],
																		deviceInfoSize);
		if (retval != CSWP_SUCCESS) {
			LOG_ERROR("Error [%d] in creating batch for cswp_device_open.", retval);
			return ERROR_FAIL;
		}
	}
	/* Submit batch for cswp_device_open */
	retval = conn->api.batch_end(conn->client, &ops_completed);
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Error [%d] Batch submission for cswp_device_open failed.",
							retval);
		return ERROR_FAIL;
	}
	if (ops_completed != conn->num_devices) {
		LOG_ERROR("Not all devices were opened.");
		return ERROR_FAIL;
	}

	LOG_INFO("All available devices are opened.");

	/* Fill in the devices data */
	conn->devices = (cswp_device_t *)malloc(conn->num_devices
																					* sizeof(cswp_device_t));
	for(size_t i = 0; i < conn->num_devices; i++) {
		conn->devices[i].name = (const char *) strndup(deviceList[i],
																										MAX_DEV_NAME_LEN);
		conn->devices[i].type = (const char *) strndup(deviceTypes[i],
																										MAX_DEV_TYPE_LEN);
		conn->devices[i].info = (const char *) strndup(deviceInfoList[i],
																										MAX_DEV_INFO_LEN);
		conn->devices[i].capabilities = capabilities[i];
		conn->devices[i].capabilities_data = capabilityData[i];
	}

	LOG_INFO("CSWP debug interface is enabled.");

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_disable
 *
 * Close all the devices and stop the CSWP server.
 * -------------------------------------------------------------------------
 */
static int
cswp_disable(cswp_conn_t* conn) {
	int retval = ERROR_OK;

	if (!conn) {
		LOG_ERROR("CSWP connection is NULL.");
		return ERROR_FAIL;
	}

	/* If CSWP is enabled we close each device, otherwise we just send
	 * the terminate command.
	 */
	if (cswp_is_enabled(conn)) {
		uint32_t ops_completed = 0;

		/* Start batch for cswp_device_close */
		retval = conn->api.batch_begin(conn->client, 1 /*abort on error*/);
		if (retval != CSWP_SUCCESS) {
			LOG_ERROR("Error [%d] in beginning batch for cswp_device_close.", retval);
			return ERROR_FAIL;
		}
		/* Create batch for cswp_device_close */
		for (uint32_t i = 0; (i < conn->num_devices); i++) {
				retval = conn->api.device_close(conn->client, i);
				if (retval != CSWP_SUCCESS) {
					LOG_ERROR("Error [%d] in creating batch for cswp_device_close.",
										retval);
					return ERROR_FAIL;
				}
		}
		/* Submit batch for cswp_device_close */
		retval = conn->api.batch_end(conn->client, &ops_completed);
		if (retval != CSWP_SUCCESS) {
			LOG_ERROR("Error [%d] Batch submission for cswp_device_close failed.",
								retval);
			return ERROR_FAIL;
		}
		if (ops_completed != conn->num_devices) {
			LOG_ERROR("Not all devices were closed.");
			return ERROR_FAIL;
		}
		LOG_INFO("All available devices are closed.");
	}

	/* Stop CSWP server */
	retval = conn->api.term(conn->client);
	if (retval != CSWP_SUCCESS) {
		LOG_ERROR("Error [%d] in terminating CSWP server.", retval);
		return ERROR_FAIL;
	}

	LOG_INFO("Connection to the CSWP server attached to %s has been terminated",
						cswp_conn_name(conn));

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_validate_dev0_mem_req
 *
 * Validate a memory request for device 0.
 * -------------------------------------------------------------------------
 */
static int
cswp_validate_dev0_mem_req(uint64_t address, uint64_t bytes_asked) {

	if (address % 4 != 0) {
		LOG_ERROR("CSWP memory access address is not aligned to 4 bytes at 0x%"
							PRIx64, address);
		return ERROR_FAIL;
	}

	if (bytes_asked > DEV0_MEM_MAX_BIN_SZ) {
		LOG_ERROR("CSWP memory access for device 0 cannot be greater than %u"
							" bytes.", (unsigned)DEV0_MEM_MAX_BIN_SZ);
		return ERROR_FAIL;
	}

	if ((bytes_asked < 4) || (bytes_asked % 4 != 0)) {
		LOG_ERROR("CSWP memory access for device 0 cannot be less than 4 bytes or"
							" not a multiple of 4");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_validate_dev1_mem_req
 *
 * Validate a memory request for device 1.
 * -------------------------------------------------------------------------
 */
static int
cswp_validate_dev1_mem_req(uint64_t address, uint64_t bytes_asked) {

	if ((((bytes_asked == 2) && (address % 2 != 0)) ||
		(((bytes_asked == 3) || (bytes_asked == 4)) && (address % 4 != 0)) ||
		((bytes_asked > 4) && (address % 8 != 0)))) {
		LOG_ERROR("CSWP memory address is not aligned as expected.");
		return ERROR_FAIL;
	}

	if (bytes_asked > DEV1_MEM_MAX_BIN_SZ) {
		LOG_ERROR("CSWP memory access for device 1 cannot be greater than %u"
							" bytes.", (unsigned)DEV1_MEM_MAX_BIN_SZ);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_create_mem_subreqs
 *
 * Create subreqs for a memory request to ensure CSWP read/write requests
 * are aligned to the access size. All the subreqs for a given request are
 * submitted as a batch request. Maximum number of subreqs is 4 considering
 * worst case alignment of request start address and its size.
 * -------------------------------------------------------------------------
 */
static uint64_t
cswp_create_mem_subreqs(mem_req_info* req_info, uint64_t rw_values_idx,
												uint64_t total_bytes) {

	uint64_t saddr		 = req_info->start_addr;
	uint64_t buf_idx	 = rw_values_idx;
	uint64_t num_bytes = total_bytes;
	uint64_t i				 = 0;

	while (num_bytes > 0) {
		req_info->subreqs[i].saddr		= saddr;
		req_info->subreqs[i].buf_idx	= buf_idx;
		if (num_bytes >= 8) {
			req_info->subreqs[i].size		= (req_info->devID == 0) ?
																			((num_bytes/4) * 4)
																			: ((num_bytes/8) * 8);
			req_info->subreqs[i].acc_sz	= (req_info->devID == 0) ?
																			CSWP_ACCESS_SIZE_32
																			: CSWP_ACCESS_SIZE_64;
		} else if (num_bytes >= 4) {
			req_info->subreqs[i].size		= ((num_bytes/4) * 4);
			req_info->subreqs[i].acc_sz	= CSWP_ACCESS_SIZE_32;
		} else if (num_bytes >= 2) {
			req_info->subreqs[i].size		= ((num_bytes/2) * 2);
			req_info->subreqs[i].acc_sz	= CSWP_ACCESS_SIZE_16;
		} else {
			req_info->subreqs[i].size		= 1;
			req_info->subreqs[i].acc_sz	= CSWP_ACCESS_SIZE_8;
		}
		/* Updates for next subreq */
		saddr = saddr + req_info->subreqs[i].size;
		buf_idx = buf_idx + req_info->subreqs[i].size;
		num_bytes = num_bytes - req_info->subreqs[i].size;
		i++;
	}
	return i;
}

/**
 * -------------------------------------------------------------------------
 * cswp_send_mem_req
 *
 * Submit a memory read/write request to CSWP. Uses local rw_values buffer
 * to store the read/write values.
 * -------------------------------------------------------------------------
 */
static int
cswp_send_mem_req(cswp_conn_t* conn, mem_req_info* req_info, bool is_read) {

	uint64_t address;
	unsigned accessSize;
	uint64_t size;
	uint64_t buf_idx;
	uint8_t *buf;
	uint32_t flags = req_info->flags;
	int retval = ERROR_OK;

	if (req_info->num_subreqs > 1) {
		retval = conn->api.batch_begin(conn->client, 1 /*abort on error*/);
		if (retval != CSWP_SUCCESS)
			return ERROR_FAIL;
	}

	for (uint32_t i = 0; (i < req_info->num_subreqs); i++) {
		address			= req_info->subreqs[i].saddr;
		accessSize	= req_info->subreqs[i].acc_sz;
		size				= req_info->subreqs[i].size;
		buf_idx			= req_info->subreqs[i].buf_idx;
		buf					= &req_info->rw_values[buf_idx];
		if (is_read) {
			size_t bytes_read = 0;
			retval = conn->api.device_mem_read(conn->client,
																					(uint32_t)(req_info->devID),
																					address, size, accessSize, flags,
																					buf, &bytes_read);
			if ((retval != CSWP_SUCCESS) || (bytes_read != size))
				return ERROR_FAIL;
		} else {
			retval = conn->api.device_mem_write(conn->client,
																					(uint32_t)(req_info->devID),
																					address, size, accessSize, flags,
																					buf);
			if (retval != CSWP_SUCCESS)
				return ERROR_FAIL;
		}
	}

	if (req_info->num_subreqs > 1) {
		uint32_t ops_completed = 0;
		retval = conn->api.batch_end(conn->client, &ops_completed);
		if ((retval != CSWP_SUCCESS) || (ops_completed != req_info->num_subreqs)) {
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_process_mem_req
 *
 * Validates the memory request, creates subreqs if needed, and submits the
 * read/write request to CSWP.
 * -------------------------------------------------------------------------
 */
static int
cswp_process_mem_req(cswp_conn_t* conn, mem_req_info* req_info, uint8_t *buf,
											bool is_read) {

	int retval = ERROR_OK;

	retval = (req_info->devID == 0) ?
						cswp_validate_dev0_mem_req(req_info->start_addr,
																												req_info->total_bytes)
						:	cswp_validate_dev1_mem_req(req_info->start_addr,
																												req_info->total_bytes);

	if (retval != ERROR_OK) {
		LOG_ERROR("CSWP read/write req is invalid.");
		return retval;
	}

	/* Due to limited size of CSWP RSP buffer, one CSWP_MEM_READ/WRITE (batch or
	 * non-batch) can only read/write MEM_MAX_RW_SZ bytes at a time. Any larger
	 * request needs to be chopped into < MEM_MAX_RW_SZ size chunks.
	 */
	uint64_t count_down_bytes = req_info->total_bytes;
	uint64_t num_chunk_bytes	= 0;
	uint64_t rw_values_idx		= 0; /* Index into rw_values buffer */

	while (count_down_bytes > 0) {

		if (count_down_bytes >= MEM_MAX_RW_SZ) {
			num_chunk_bytes	= MEM_MAX_RW_SZ;
		} else {
			num_chunk_bytes	= count_down_bytes;
		}

		/* Each req chunk can further be split into upto 4 subreqs to ensure REQ
		 * size is always a multiple of access_size. Request with more than 1
		 * subreq will be submitted as a batch request.
		 */
		req_info->num_subreqs = cswp_create_mem_subreqs(req_info, rw_values_idx,
																										num_chunk_bytes);
		retval = cswp_send_mem_req(conn, req_info, is_read);

		if (retval != ERROR_OK) { return retval; }

		req_info->start_addr = req_info->start_addr	+ num_chunk_bytes;
		rw_values_idx				= rw_values_idx				 + num_chunk_bytes;
		count_down_bytes		 = count_down_bytes			- num_chunk_bytes;
	}
	/* Copy the read bytes to the output buffer if this is a read request */
	if (is_read) {
		memcpy(buf, req_info->rw_values, req_info->total_bytes);
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_process_reg_req
 *
 * Validates the register read/write request and submits the request
 * to CSWP.
 * -------------------------------------------------------------------------
 */
static int
cswp_process_reg_req(cswp_conn_t* conn, uint64_t devID, uint64_t address,
											uint64_t bytes_asked, uint8_t *buf, bool is_read) {

	if (bytes_asked != 4 && bytes_asked != 8) {
		LOG_ERROR("CSWP reg read/write size of device 2 can only be 4 or 8 bytes.");
		return ERROR_FAIL;
	}

	/* All registers are accesses as multiple of 32b, hence array of size 2 */
	uint32_t registerID[2];	/* Register address for reg read/write requests */
	uint32_t registerValues[2]; /* Stored read values for reg read requests and
																	write values for reg write requests */

	registerID[0]			 = (uint32_t)address;
	registerID[1]						= (uint32_t)(address + 4); /* Discarded if 32b
																												read/write */
	size_t	 num_32b_reg_acc = (bytes_asked == 4) ? 1 : 2;
	int	retval;

	if (is_read) {
		retval = conn->api.device_reg_read(conn->client, (uint32_t)devID,
																			 num_32b_reg_acc, registerID,
											 registerValues, num_32b_reg_acc);
		if (retval != CSWP_SUCCESS) {
			return ERROR_FAIL;
		}
		/* Split the two 32b values into byte array */
		h_u32_to_le(&buf[0], registerValues[0]);			/* First 4 bytes (low part) */
		if (num_32b_reg_acc == 2) {
			h_u32_to_le(&buf[4], registerValues[1]);	/* Next 4 bytes (high part) */
		}
	} else {
		/* Merge the bytes in 32b values */
		registerValues[0] = le_to_h_u32(&buf[0]);		 /* First 4 bytes (low part) */
		if (num_32b_reg_acc == 2) {
			registerValues[1] = le_to_h_u32(&buf[4]); /* Next 4 bytes (high part) */
		}
		retval = conn->api.device_reg_write(conn->client, (uint32_t)devID,
																				num_32b_reg_acc, registerID,
											registerValues, num_32b_reg_acc);
		if (retval != CSWP_SUCCESS) {
			return ERROR_FAIL;
		}
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_mem_ap_read
 * -------------------------------------------------------------------------
 */
int
cswp_mem_ap_read(cswp_conn_t* conn, uint64_t devID, uint64_t address,
									uint32_t acc_bytes, uint32_t num_accesses, uint8_t* buf,
									bool incr) {
	if (!cswp_is_enabled(conn))	{
		LOG_ERROR("CSWP is not enabled.");
		return ERROR_FAIL;
	}
	if (devID >= conn->num_devices) {
		LOG_ERROR("CSWP device ID is out of range.");
		return ERROR_FAIL;
	}
	if (!buf) {
		LOG_ERROR("CSWP read buffer pointer is NULL.");
		return ERROR_FAIL;
	}

	int	retval = ERROR_OK;
	uint64_t bytes_asked = (acc_bytes * num_accesses);

	if ((conn->devices[devID].capabilities & CSWP_CAP_MEM) != 0) {
		/* Prepare the memory request info */
		mem_req_info req_info;
		req_info.devID				= (uint32_t)devID;
		req_info.start_addr		= address;
		req_info.total_bytes	= bytes_asked;
		req_info.num_subreqs	= 0;
		req_info.flags				= incr ? 0 : CSWP_MEM_NO_ADDR_INC;
		retval = cswp_process_mem_req(conn, &req_info, buf, true /* is_read*/);
		LOG_DEBUG_IO("Read %" PRIu64 "B from dev%" PRId64 " address 0x%" PRIx64
									" incr %d using MEM, result: %d.", bytes_asked, devID,
									address, incr, retval);
	} else if ((conn->devices[devID].capabilities & CSWP_CAP_REG) != 0) {
		if (incr) {
			retval = cswp_process_reg_req(conn, devID, address, bytes_asked, buf,
																		true /* is_read*/);
		} else {
			uint32_t remains = num_accesses;
			while (remains > 0) {
				retval = cswp_process_reg_req(conn, devID, address, acc_bytes, buf,
																			true /* is_read*/);
				if (retval != ERROR_OK)
					break;
				buf += acc_bytes;
				remains--;
			}
		}
		LOG_DEBUG_IO("Read %" PRIu64 "B from dev%" PRId64 " address 0x%" PRIx64
									" incr %d using REG, result: %d.", bytes_asked, devID,
									address, incr, retval);
	} else {
		LOG_ERROR("Device %s does not support memory and register accesses.",
							conn->devices[devID].name);
		return ERROR_FAIL;
	}

	if (retval != ERROR_OK) {
		LOG_ERROR("CSWP read failed at 0x%" PRIx64 ": %d", address, retval);
		return ERROR_FAIL;
	}

	if (acc_bytes == 4) {
		for(uint64_t i=0; i < bytes_asked; i+=acc_bytes) {
			uint32_t v = buf[i] | (buf[i+1]<<8) | (buf[i+2]<<16) | (buf[i+3]<<24);
			LOG_DEBUG_IO("\tData read: 0x%08x", v);
		}
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_mem_ap_write
 * -------------------------------------------------------------------------
 */
int
cswp_mem_ap_write(cswp_conn_t* conn, uint64_t devID, uint64_t address,
									uint32_t acc_bytes, uint32_t num_accesses,
									const uint8_t* buf, bool incr) {
	if (!cswp_is_enabled(conn))	 {
		LOG_ERROR("CSWP is not enabled.");
		return ERROR_FAIL;
	}
	if (devID >= conn->num_devices) {
		LOG_ERROR("CSWP device ID is out of range.");
		return ERROR_FAIL;
	}

	int retval = ERROR_OK;
	uint64_t bytes_asked = (acc_bytes * num_accesses);

	if ((conn->devices[devID].capabilities & CSWP_CAP_MEM) != 0) {
		/* Prepare the memory request info */
		mem_req_info req_info;
		req_info.devID				= (uint32_t)devID;
		req_info.start_addr		= address;
		req_info.total_bytes	= bytes_asked;
		req_info.num_subreqs	= 0;
		req_info.flags				= incr ? 0 : CSWP_MEM_NO_ADDR_INC;
		/* Copy the write data to the input buffer */
		for (size_t i = 0; i < bytes_asked; i++)
			req_info.rw_values[i] = buf[i];
		retval = cswp_process_mem_req(conn, &req_info, (uint8_t *)buf,
																	false /* is_read*/);
		LOG_DEBUG_IO("Wrote %" PRIu64 "B to dev%" PRId64 " address 0x%" PRIx64
									" incr %d using MEM, result: %d.", bytes_asked, devID,
									address, incr, retval);
	} else if ((conn->devices[devID].capabilities & CSWP_CAP_REG) != 0) {
		if (incr) {
			retval = cswp_process_reg_req(conn, devID, address, bytes_asked,
																		(uint8_t *)buf, false /* is_read*/);
		} else {
			uint32_t remains = num_accesses;
			while (remains > 0) {
				retval = cswp_process_reg_req(conn, devID, address, acc_bytes,
																			(uint8_t *)buf,	false /* is_read*/);
				if (retval != ERROR_OK)
					break;
				buf += acc_bytes;
				remains--;
			}
		}
		LOG_DEBUG_IO("Wrote %" PRIu64 "B to dev%" PRId64 " address 0x%" PRIx64
									" incr %d using REG, result %d.", bytes_asked, devID, address,
									incr, retval);
	} else {
		LOG_ERROR("Device %s does not support memory and register accesses.",
							conn->devices[devID].name);
		return ERROR_FAIL;
	}

	if (retval != ERROR_OK) {
		LOG_ERROR("CSWP write failed at 0x%" PRIx64 ": %d", address, retval);
		return ERROR_FAIL;
	}

	if (acc_bytes == 4) {
		for(uint64_t i=0; i < bytes_asked; i+=acc_bytes) {
			uint32_t v = buf[i] | (buf[i+1]<<8) | (buf[i+2]<<16) | (buf[i+3]<<24);
			LOG_DEBUG_IO("\tData written: 0x%08x", v);
		}
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_find_connection
 *
 * Find a CSWP connection by name.
 * -------------------------------------------------------------------------
 */
cswp_conn_t*
cswp_find_connection(const char* name) {
	cswp_conn_entry_t* entry;
	list_for_each_entry(entry, &cswp_connections, list) {
		if (strcmp(entry->name, name) == 0) {
			return entry->conn;
		}
	}
	return NULL;
}

/**
 * -------------------------------------------------------------------------
 * cswp_create_connection
 *
 * Create a new CSWP connection.
 * -------------------------------------------------------------------------
 */
static cswp_conn_t*
cswp_create_connection(const char* name) {
	/* Check if connection already exists */
	if (cswp_find_connection(name)) {
		LOG_ERROR("CSWP connection '%s' already exists.", name);
		return NULL;
	}

	/* Create new connection */
	cswp_conn_t* conn = calloc(1, sizeof(cswp_conn_t));
	if (!conn) {
		LOG_ERROR("Failed to allocate CSWP connection.");
		return NULL;
	}

	/* Add to registry */
	cswp_conn_entry_t* entry = calloc(1, sizeof(cswp_conn_entry_t));
	if (!entry) {
		LOG_ERROR("Failed to allocate CSWP connection entry.");
		free(conn);
		return NULL;
	}

	entry->name = strdup(name);
	if (!entry->name) {
		LOG_ERROR("Failed to allocate CSWP connection name.");
		free(entry);
		free(conn);
		return NULL;
	}

	entry->conn = conn;
	list_add_tail(&entry->list, &cswp_connections);

	LOG_INFO("Created CSWP connection '%s.'", name);
	return conn;
}

/**
 * -------------------------------------------------------------------------
 * cswp_close_connection
 *
 * Clean up given CSWP connection and stop the CSWP server.
 * -------------------------------------------------------------------------
 */
static int
cswp_close_connection(cswp_conn_t* conn) {
	if (!conn) {
		LOG_ERROR("Cannot close CSWP connection. CSWP connection is NULL");
		return ERROR_FAIL;
	}

	int retval = cswp_disable(conn);
	if (retval != ERROR_OK) {
		LOG_ERROR("Error [%d] in stopping CSWP server for connection.", retval);
		return retval;
	}

	conn->api.delete_client(conn->client); /* Delete CSWP client context */
	conn->client	= NULL; /* Set CSWP client context to NULL */
	conn->enabled = false; /* Set CSWP debug interface to disabled */

	/* Free allocated strings */
	if (conn->conn_params.cfg_str) {
		free(conn->conn_params.cfg_str);
	}

	/* Close the library handle */
	if (conn->lib_handle) {
		dlclose(conn->lib_handle);
		conn->lib_handle = NULL;
	}

	LOG_DEBUG("CSWP connection %p closed.", conn);
	free(conn);
	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * cswp_cleanup_all
 *
 * Clean up all CSWP connections.
 * -------------------------------------------------------------------------
 */
static int
cswp_cleanup_all(void) {
	cswp_conn_entry_t *entry, *tmp;
	int retval = ERROR_OK;
	LOG_DEBUG("Shutting down CSWP...");
	list_for_each_entry_safe(entry, tmp, &cswp_connections, list) {
		if (entry->conn) {
			LOG_INFO("Closing CSWP connection %s...", entry->name);
			retval = cswp_close_connection(entry->conn);
			if (retval != ERROR_OK) {
				LOG_ERROR("Error [%d] in closing CSWP connection '%s'.", retval,
									entry->name);
				return retval;
			}
		}
		free(entry->name);
		list_del(&entry->list);
		free(entry);
	}
	if (cswp_library_path != NULL)
		free(cswp_library_path);
	LOG_DEBUG("Shutting down CSWP: done.");
	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * handle_cswp_client_lib_command
 *
 * Select the CSWP client library to use. It does not load it and must be
 * called before CSWP new which will load it.
 * -------------------------------------------------------------------------
 */
COMMAND_HANDLER(handle_cswp_client_lib_command) {
	if (CMD_ARGC > 1) {
		LOG_ERROR("Invalid argument to the cswp client_lib command, expect a"
								" single path");
		return ERROR_COMMAND_SYNTAX_ERROR;
	} else if (CMD_ARGC == 0) {
		if (cswp_library_path == NULL)
			command_print(CMD, "No client library set.");
		else
			command_print(CMD, "%s", cswp_library_path);
		return ERROR_OK;
	}
	if (cswp_library_path != NULL)
		free (cswp_library_path);
	cswp_library_path = strdup (CMD_ARGV[0]);
	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * handle_cswp_new_command
 *
 * Enable CSWP for given connection. This loads the CSWP client library,
 * starts the CSWP server, and gets device information and capabilities.
 * -------------------------------------------------------------------------
 */
COMMAND_HANDLER(handle_cswp_new_command) {
	int retval;

	if (CMD_ARGC < 3) {
		LOG_ERROR("Usage: cswp new <conn_name> {-usb <vid:pid:sid:iid> | -tcp <host:port>}");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	if (cswp_library_path == NULL) {
		LOG_ERROR("The path to the client library must be set first with 'cswp client_lib <path>'");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	const char *name = CMD_ARGV[0];
	cswp_conn_t* conn = cswp_create_connection(name);
	if (!conn) {
		LOG_ERROR("Failed to create CSWP connection.");
		return ERROR_FAIL;
	}

	/* Parse remaining arguments */
	for (int i = 1; i < (int)CMD_ARGC; i++) {
		if (strcmp(CMD_ARGV[i], "-usb") == 0) {
			if (i + 1 >= (int)CMD_ARGC) {
				command_print(CMD, "Missing connection string after -usb.");
				return ERROR_COMMAND_SYNTAX_ERROR;
			}
			conn->conn_params.type = "usb";
			conn->conn_params.cfg_str = strdup(CMD_ARGV[i + 1]);
			if (!conn->conn_params.cfg_str) {
				command_print(CMD, "Failed to allocate memory for USB connection"
														" string");
				return ERROR_FAIL;
			}
			i++; /* Skip next argument */
		} else if (strcmp(CMD_ARGV[i], "-tcp") == 0) {
			if (i + 1 >= (int)CMD_ARGC) {
				command_print(CMD, "Missing connection string after -tcp.");
				return ERROR_COMMAND_SYNTAX_ERROR;
			}
			conn->conn_params.type = "tcp";
			conn->conn_params.cfg_str = strdup(CMD_ARGV[i + 1]);
			if (!conn->conn_params.cfg_str) {
				command_print(CMD, "Failed to allocate memory for TCP connection"
														" string.");
				return ERROR_FAIL;
			}
			i++; /* Skip next argument */
		}
	}

	/* Require explicit connection type */
	if ((!conn->conn_params.type) ||
			((strcmp(conn->conn_params.type, "tcp") != 0)
					&& (strcmp(conn->conn_params.type, "usb") != 0))) {
		command_print(CMD, "Missing connection type: specify either -usb"
												" <vid:pid:sid:iid> or -tcp <host:port>.");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* Load CSWP client library */
	retval = cswp_load_client_lib(conn, cswp_library_path);
	if (retval != ERROR_OK) {
		command_print(CMD, "Failed to load CSWP client library.");
		return retval;
	}

	/* Create CSWP client context */
	conn->client = conn->api.create_client(conn->conn_params.type,
																					conn->conn_params.cfg_str);
	if (!conn->client) {
		LOG_ERROR("Failed to create CSWP client context");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * handle_cswp_disable_command
 *
 * Disable CSWP for given connection.
 * -------------------------------------------------------------------------
 */
COMMAND_HANDLER(handle_cswp_disable_command) {

	if (CMD_ARGC != 1) {
		command_print(CMD, "usage: cswp disable <conn_name>");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	const char *name = CMD_ARGV[0];
	cswp_conn_t* conn = cswp_find_connection(name);
	if (!conn) {
		command_print(CMD, "CSWP connection not found.");
		return ERROR_FAIL;
	}

	int retval = cswp_disable(conn);
	if (retval != ERROR_OK) {
		command_print(CMD, "Failed to disable CSWP.");
		return retval;
	}
	command_print(CMD, "CSWP Status: DISABLED");
	return ERROR_OK;
}

/**
 * -------------------------------------------------------------------------
 * handle_cswp_status_command
 *
 * Show the status of CSWP.
 * -------------------------------------------------------------------------
 */
COMMAND_HANDLER(handle_cswp_status_command) {

	if (CMD_ARGC != 1) {
		command_print(CMD, "usage: cswp status <conn_name>");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	const char *name = CMD_ARGV[0];
	cswp_conn_t* conn = cswp_find_connection(name);
	if (!conn) {
		command_print(CMD, "CSWP connection not found.");
		return ERROR_FAIL;
	}

	if (cswp_is_enabled(conn)) {
		command_print(CMD, "CSWP Status: ENABLED");
		command_print(CMD, "	Connection: %s %s",
									conn->conn_params.type ? conn->conn_params.type : "unknown",
									conn->conn_params.cfg_str ? conn->conn_params.cfg_str
																							: "unknown");
		command_print(CMD, "	Library: %s",
									conn->lib_handle ? "loaded" : "unloaded");
		command_print(CMD, "	Client: %s",
									conn->client ? "initialized" : "not initialized");
	} else {
		command_print(CMD, "CSWP Status: DISABLED");
	}

	return ERROR_OK;
}

static const struct command_registration cswp_subcommands[] = {
	{
		.name = "new",
		.handler = handle_cswp_new_command,
		.mode = COMMAND_CONFIG,
		.help = "Enable CSWP with specified library and connection type",
		.usage = "<conn_name> {-tcp <host:port> |"
							" -usb <vid:pid:sid:iid>}",
	},
	{
		.name		= "disable",
		.handler = handle_cswp_disable_command,
		.mode		= COMMAND_ANY,
		.help		= "Disable CSWP",
		.usage	 = "<conn_name>",
	},
	{
		.name		= "status",
		.handler = handle_cswp_status_command,
		.mode		= COMMAND_ANY,
		.help		= "Show CSWP status",
		.usage	 = "<conn_name>",
	},
	{
		.name		= "client_lib",
		.handler = handle_cswp_client_lib_command,
		.mode		= COMMAND_CONFIG,
		.help		= "Display or set the path to the CSWP client library",
		.usage	 = "[ <path> ]",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration cswp_commands[] = {
	{
		.name = "cswp",
		.mode = COMMAND_ANY,
		.help = "CoreSight Wire Protocol commands",
		.usage = "",
		.chain = cswp_subcommands,
	},
	COMMAND_REGISTRATION_DONE
};

/**
 * -------------------------------------------------------------------------
 * cswp_conn_name
 *
 * Return the CSWP connection name
 * -------------------------------------------------------------------------
 */
const char *cswp_conn_name(struct cswp_conn* conn) {
	cswp_conn_entry_t* entry;
	list_for_each_entry(entry, &cswp_connections, list) {
		if (entry->conn == conn) {
			return (const char *)entry->name;
		}
	}
	return "not found";
}

/**
 * -------------------------------------------------------------------------
 * transport code for CSWP, this code isn't really used but OpenOCD expects
 * a transport
 * -------------------------------------------------------------------------
 */
static int cswp_select(struct command_context *ctx)
{
	return register_commands(ctx, NULL, cswp_commands);
}

static int cswp_init(struct command_context *cmd_ctx)
{
	cswp_conn_entry_t *entry;

	list_for_each_entry(entry, &cswp_connections, list) {
		int retval = ERROR_OK;
		cswp_conn_t *conn = entry->conn;

		/* Enable CSWP and open all devices */
		retval = cswp_enable(conn);
		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to enable CSWP connection %s.", entry->name);
			return retval;
		}
		LOG_INFO("CSWP connection %s using %s %s successfully enabled",
									entry->name,
									conn->conn_params.type ? conn->conn_params.type : "unknown",
									conn->conn_params.cfg_str ? conn->conn_params.cfg_str
																							: "unknown");

		/* Print device information and capabilities */
		LOG_DEBUG("\tDevice information and capabilities:");
		for (size_t i = 0; i < conn->num_devices; ++i) {
			LOG_DEBUG("\tDevice: %zu", i);
			LOG_DEBUG("\t\tType: %s, Name: %s, Info: %s", conn->devices[i].type,
										conn->devices[i].name, conn->devices[i].info);
			LOG_DEBUG("\t\tCapabilities:");
			if ((conn->devices[i].capabilities & CSWP_CAP_REG) == CSWP_CAP_REG)
				LOG_DEBUG("\t\t\tCSWP_CAP_REG (MaxNumRegs: %u)",
											conn->devices[i].capabilities_data);
			if ((conn->devices[i].capabilities & CSWP_CAP_MEM) == CSWP_CAP_MEM)
				LOG_DEBUG("\t\t\tCSWP_CAP_MEM");
			if ((conn->devices[i].capabilities & CSWP_CAP_MEM_POLL) == CSWP_CAP_MEM_POLL)
				LOG_DEBUG("\t\t\tCSWP_CAP_MEM_POLL");
		}
	}
	return ERROR_OK;
}

static struct transport cswp_transport = {
	.name = "cswp",
	.select = cswp_select,
	.init = cswp_init,
	.terminate = cswp_cleanup_all,
};

static void cswp_constructor(void) __attribute__((constructor));
static void cswp_constructor(void)
{
	transport_register(&cswp_transport);
}

/**
 * -------------------------------------------------------------------------
 * transport_is_cswp
 *
 * Return true if CSWP is the active transport
 * -------------------------------------------------------------------------
 */
bool transport_is_cswp(void)
{
	return get_current_transport() == &cswp_transport;
}
