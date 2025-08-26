/*
 * Copyright (c) 2004-2009 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2007 Xsigo Systems Inc.  All rights reserved.
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
 * Copyright (c) 2011 Mellanox Technologies LTD.  All rights reserved.
 * Copyright (c) 2025 SuperLinear Lab.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <inttypes.h>

#include <infiniband/umad.h>
#include <infiniband/mad.h>

#include "ibdiag_common.h"

#define MAX_GUIDS 1000
#define MAX_LINE_LENGTH 256
#define MAX_OUTPUT_SIZE 8192
#define DEFAULT_CONFIG_FILE "conf/pperfquery.conf"
#define DEFAULT_OUTPUT_FILE "pperfquery_output.txt"

#define ALL_PORTS 0xFF
#define MAX_PORTS 255

static struct ibmad_port *srcport;
static struct ibmad_ports_pair *srcports;

typedef struct {
	uint32_t portselect;
	uint32_t counterselect;
	uint32_t symbolerrors;
	uint32_t linkrecovers;
	uint32_t linkdowned;
	uint32_t rcverrors;
	uint32_t rcvremotephyerrors;
	uint32_t rcvswrelayerrors;
	uint32_t xmtdiscards;
	uint32_t xmtconstrainterrors;
	uint32_t rcvconstrainterrors;
	uint32_t linkintegrityerrors;
	uint32_t excbufoverrunerrors;
	uint32_t qp1dropped;
	uint32_t vl15dropped;
	uint32_t xmtdata;
	uint32_t rcvdata;
	uint32_t xmtpkts;
	uint32_t rcvpkts;
	uint32_t xmtwait;
} perf_count_t;

typedef struct {
	uint32_t portselect;
	uint32_t counterselect;
	uint64_t portxmitdata;
	uint64_t portrcvdata;
	uint64_t portxmitpkts;
	uint64_t portrcvpkts;
	uint64_t portunicastxmitpkts;
	uint64_t portunicastrcvpkts;
	uint64_t portmulticastxmitpkits;
	uint64_t portmulticastrcvpkts;

	uint32_t counterSelect2;
	uint64_t symbolErrorCounter;
	uint64_t linkErrorRecoveryCounter;
	uint64_t linkDownedCounter;
	uint64_t portRcvErrors;
	uint64_t portRcvRemotePhysicalErrors;
	uint64_t portRcvSwitchRelayErrors;
	uint64_t portXmitDiscards;
	uint64_t portXmitConstraintErrors;
	uint64_t portRcvConstraintErrors;
	uint64_t localLinkIntegrityErrors;
	uint64_t excessiveBufferOverrunErrors;
	uint64_t VL15Dropped;
	uint64_t portXmitWait;
	uint64_t QP1Dropped;
} perf_count_ext_t;

// Thread data structure
typedef struct {
	int thread_id;
	uint64_t guid;
	char output_buffer[MAX_OUTPUT_SIZE];
	int output_size;
	int extended;
	int timeout;
	int verbose;
} thread_data_t;

// Globals
static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rpc_mutex = PTHREAD_MUTEX_INITIALIZER; // protect libibmad calls
static FILE *output_file = NULL;
static int num_guids = 0;
static uint64_t guids[MAX_GUIDS];

// ---------- small safe helpers ----------
static int appendf(char *dst, int cap, int *poff, const char *fmt, ...)
{
	if (*poff >= cap) return -1;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst + *poff, cap - *poff, fmt, ap);
	va_end(ap);
	if (n < 0) return -1;
	if (*poff + n >= cap) { *poff = cap; return -1; }
	*poff += n;
	return 0;
}

static inline void aggregate_4bit(uint32_t *dest, uint32_t val)
{
	uint32_t sum = *dest + val;
	*dest = (sum > 0xF || sum < *dest) ? 0xF : sum;
}
static inline void aggregate_8bit(uint32_t *dest, uint32_t val)
{
	uint32_t sum = *dest + val;
	*dest = (sum > 0xFF || sum < *dest) ? 0xFF : sum;
}
static inline void aggregate_16bit(uint32_t *dest, uint32_t val)
{
	uint32_t sum = *dest + val;
	*dest = (sum > 0xFFFF || sum < *dest) ? 0xFFFF : sum;
}
static inline void aggregate_32bit(uint32_t *dest, uint32_t val)
{
	uint32_t sum = *dest + val;
	*dest = (sum < *dest) ? 0xFFFFFFFFu : sum;
}
static inline void aggregate_64bit(uint64_t *dest, uint64_t val)
{
	uint64_t sum = *dest + val;
	*dest = (sum < *dest) ? 0xFFFFFFFFFFFFFFFFULL : sum;
}

// ---------- per-thread aggregation ----------
static void aggregate_perfcounters(uint8_t *pc, perf_count_t *acc)
{
	uint32_t val;

	mad_decode_field(pc, IB_PC_PORT_SELECT_F, &val);
	acc->portselect = val;
	mad_decode_field(pc, IB_PC_COUNTER_SELECT_F, &val);
	acc->counterselect = val;
	mad_decode_field(pc, IB_PC_ERR_SYM_F, &val);
	aggregate_16bit(&acc->symbolerrors, val);
	mad_decode_field(pc, IB_PC_LINK_RECOVERS_F, &val);
	aggregate_8bit(&acc->linkrecovers, val);
	mad_decode_field(pc, IB_PC_LINK_DOWNED_F, &val);
	aggregate_8bit(&acc->linkdowned, val);
	mad_decode_field(pc, IB_PC_ERR_RCV_F, &val);
	aggregate_16bit(&acc->rcverrors, val);
	mad_decode_field(pc, IB_PC_ERR_PHYSRCV_F, &val);
	aggregate_16bit(&acc->rcvremotephyerrors, val);
	mad_decode_field(pc, IB_PC_ERR_SWITCH_REL_F, &val);
	aggregate_16bit(&acc->rcvswrelayerrors, val);
	mad_decode_field(pc, IB_PC_XMT_DISCARDS_F, &val);
	aggregate_16bit(&acc->xmtdiscards, val);
	mad_decode_field(pc, IB_PC_ERR_XMTCONSTR_F, &val);
	aggregate_8bit(&acc->xmtconstrainterrors, val);
	mad_decode_field(pc, IB_PC_ERR_RCVCONSTR_F, &val);
	aggregate_8bit(&acc->rcvconstrainterrors, val);
	mad_decode_field(pc, IB_PC_ERR_LOCALINTEG_F, &val);
	aggregate_4bit(&acc->linkintegrityerrors, val);
	mad_decode_field(pc, IB_PC_ERR_EXCESS_OVR_F, &val);
	aggregate_4bit(&acc->excbufoverrunerrors, val);
	mad_decode_field(pc, IB_PC_QP1_DROP_F, &val);
	aggregate_16bit(&acc->qp1dropped, val);
	mad_decode_field(pc, IB_PC_VL15_DROPPED_F, &val);
	aggregate_16bit(&acc->vl15dropped, val);
	mad_decode_field(pc, IB_PC_XMT_BYTES_F, &val);
	aggregate_32bit(&acc->xmtdata, val);
	mad_decode_field(pc, IB_PC_RCV_BYTES_F, &val);
	aggregate_32bit(&acc->rcvdata, val);
	mad_decode_field(pc, IB_PC_XMT_PKTS_F, &val);
	aggregate_32bit(&acc->xmtpkts, val);
	mad_decode_field(pc, IB_PC_RCV_PKTS_F, &val);
	aggregate_32bit(&acc->rcvpkts, val);
	mad_decode_field(pc, IB_PC_XMT_WAIT_F, &val);
	aggregate_32bit(&acc->xmtwait, val);
}

static void encode_aggregate_perfcounters(uint8_t *pc, const perf_count_t *acc)
{
	uint32_t val = ALL_PORTS;
	mad_encode_field(pc, IB_PC_PORT_SELECT_F, &val);
	mad_encode_field(pc, IB_PC_COUNTER_SELECT_F, (uint32_t *)&acc->counterselect);
	mad_encode_field(pc, IB_PC_ERR_SYM_F, (uint32_t *)&acc->symbolerrors);
	mad_encode_field(pc, IB_PC_LINK_RECOVERS_F, (uint32_t *)&acc->linkrecovers);
	mad_encode_field(pc, IB_PC_LINK_DOWNED_F, (uint32_t *)&acc->linkdowned);
	mad_encode_field(pc, IB_PC_ERR_RCV_F, (uint32_t *)&acc->rcverrors);
	mad_encode_field(pc, IB_PC_ERR_PHYSRCV_F, (uint32_t *)&acc->rcvremotephyerrors);
	mad_encode_field(pc, IB_PC_ERR_SWITCH_REL_F, (uint32_t *)&acc->rcvswrelayerrors);
	mad_encode_field(pc, IB_PC_XMT_DISCARDS_F, (uint32_t *)&acc->xmtdiscards);
	mad_encode_field(pc, IB_PC_ERR_XMTCONSTR_F, (uint32_t *)&acc->xmtconstrainterrors);
	mad_encode_field(pc, IB_PC_ERR_RCVCONSTR_F, (uint32_t *)&acc->rcvconstrainterrors);
	mad_encode_field(pc, IB_PC_ERR_LOCALINTEG_F, (uint32_t *)&acc->linkintegrityerrors);
	mad_encode_field(pc, IB_PC_ERR_EXCESS_OVR_F, (uint32_t *)&acc->excbufoverrunerrors);
	mad_encode_field(pc, IB_PC_QP1_DROP_F, (uint32_t *)&acc->qp1dropped);
	mad_encode_field(pc, IB_PC_VL15_DROPPED_F, (uint32_t *)&acc->vl15dropped);
	mad_encode_field(pc, IB_PC_XMT_BYTES_F, (uint32_t *)&acc->xmtdata);
	mad_encode_field(pc, IB_PC_RCV_BYTES_F, (uint32_t *)&acc->rcvdata);
	mad_encode_field(pc, IB_PC_XMT_PKTS_F, (uint32_t *)&acc->xmtpkts);
	mad_encode_field(pc, IB_PC_RCV_PKTS_F, (uint32_t *)&acc->rcvpkts);
	mad_encode_field(pc, IB_PC_XMT_WAIT_F, (uint32_t *)&acc->xmtwait);
}

static void aggregate_perfcounters_ext(__be16 cap_mask, uint32_t cap_mask2, uint8_t *pc, perf_count_ext_t *acc)
{
	uint32_t val;
	uint64_t val64;

	mad_decode_field(pc, IB_PC_EXT_PORT_SELECT_F, &val);
	acc->portselect = val;
	mad_decode_field(pc, IB_PC_EXT_COUNTER_SELECT_F, &val);
	acc->counterselect = val;
	mad_decode_field(pc, IB_PC_EXT_XMT_BYTES_F, &val64);
	aggregate_64bit(&acc->portxmitdata, val64);
	mad_decode_field(pc, IB_PC_EXT_RCV_BYTES_F, &val64);
	aggregate_64bit(&acc->portrcvdata, val64);
	mad_decode_field(pc, IB_PC_EXT_XMT_PKTS_F, &val64);
	aggregate_64bit(&acc->portxmitpkts, val64);
	mad_decode_field(pc, IB_PC_EXT_RCV_PKTS_F, &val64);
	aggregate_64bit(&acc->portrcvpkts, val64);

	if (cap_mask & IB_PM_EXT_WIDTH_SUPPORTED) {
		mad_decode_field(pc, IB_PC_EXT_XMT_UPKTS_F, &val64);
		aggregate_64bit(&acc->portunicastxmitpkts, val64);
		mad_decode_field(pc, IB_PC_EXT_RCV_UPKTS_F, &val64);
		aggregate_64bit(&acc->portunicastrcvpkts, val64);
		mad_decode_field(pc, IB_PC_EXT_XMT_MPKTS_F, &val64);
		aggregate_64bit(&acc->portmulticastxmitpkits, val64);
		mad_decode_field(pc, IB_PC_EXT_RCV_MPKTS_F, &val64);
		aggregate_64bit(&acc->portmulticastrcvpkts, val64);
	}

	if (cap_mask2 & IB_PM_IS_ADDL_PORT_CTRS_EXT_SUP) {
		mad_decode_field(pc, IB_PC_EXT_COUNTER_SELECT2_F, &val);
		acc->counterSelect2 = val;
		mad_decode_field(pc, IB_PC_EXT_ERR_SYM_F, &val64);
		aggregate_64bit(&acc->symbolErrorCounter, val64);
		mad_decode_field(pc, IB_PC_EXT_LINK_RECOVERS_F, &val64);
		aggregate_64bit(&acc->linkErrorRecoveryCounter, val64);
		mad_decode_field(pc, IB_PC_EXT_LINK_DOWNED_F, &val64);
		aggregate_64bit(&acc->linkDownedCounter, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_RCV_F, &val64);
		aggregate_64bit(&acc->portRcvErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_PHYSRCV_F, &val64);
		aggregate_64bit(&acc->portRcvRemotePhysicalErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_SWITCH_REL_F, &val64);
		aggregate_64bit(&acc->portRcvSwitchRelayErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_XMT_DISCARDS_F, &val64);
		aggregate_64bit(&acc->portXmitDiscards, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_XMTCONSTR_F, &val64);
		aggregate_64bit(&acc->portXmitConstraintErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_RCVCONSTR_F, &val64);
		aggregate_64bit(&acc->portRcvConstraintErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_LOCALINTEG_F, &val64);
		aggregate_64bit(&acc->localLinkIntegrityErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_ERR_EXCESS_OVR_F, &val64);
		aggregate_64bit(&acc->excessiveBufferOverrunErrors, val64);
		mad_decode_field(pc, IB_PC_EXT_VL15_DROPPED_F, &val64);
		aggregate_64bit(&acc->VL15Dropped, val64);
		mad_decode_field(pc, IB_PC_EXT_XMT_WAIT_F, &val64);
		aggregate_64bit(&acc->portXmitWait, val64);
		mad_decode_field(pc, IB_PC_EXT_QP1_DROP_F, &val64);
		aggregate_64bit(&acc->QP1Dropped, val64);
	}
}

static void encode_aggregate_perfcounters_ext(uint8_t *pc, __be16 cap_mask, uint32_t cap_mask2, const perf_count_ext_t *acc)
{
	uint32_t val = ALL_PORTS;

	mad_encode_field(pc, IB_PC_EXT_PORT_SELECT_F, &val);
	mad_encode_field(pc, IB_PC_EXT_COUNTER_SELECT_F, (uint32_t *)&acc->counterselect);
	mad_encode_field(pc, IB_PC_EXT_XMT_BYTES_F, (uint64_t *)&acc->portxmitdata);
	mad_encode_field(pc, IB_PC_EXT_RCV_BYTES_F, (uint64_t *)&acc->portrcvdata);
	mad_encode_field(pc, IB_PC_EXT_XMT_PKTS_F, (uint64_t *)&acc->portxmitpkts);
	mad_encode_field(pc, IB_PC_EXT_RCV_PKTS_F, (uint64_t *)&acc->portrcvpkts);

	if (cap_mask & IB_PM_EXT_WIDTH_SUPPORTED) {
		mad_encode_field(pc, IB_PC_EXT_XMT_UPKTS_F, (uint64_t *)&acc->portunicastxmitpkts);
		mad_encode_field(pc, IB_PC_EXT_RCV_UPKTS_F, (uint64_t *)&acc->portunicastrcvpkts);
		mad_encode_field(pc, IB_PC_EXT_XMT_MPKTS_F, (uint64_t *)&acc->portmulticastxmitpkits);
		mad_encode_field(pc, IB_PC_EXT_RCV_MPKTS_F, (uint64_t *)&acc->portmulticastrcvpkts);
	}

	if (cap_mask2 & IB_PM_IS_ADDL_PORT_CTRS_EXT_SUP) {
		mad_encode_field(pc, IB_PC_EXT_COUNTER_SELECT2_F, (uint32_t *)&acc->counterSelect2);
		mad_encode_field(pc, IB_PC_EXT_ERR_SYM_F, (uint64_t *)&acc->symbolErrorCounter);
		mad_encode_field(pc, IB_PC_EXT_LINK_RECOVERS_F, (uint64_t *)&acc->linkErrorRecoveryCounter);
		mad_encode_field(pc, IB_PC_EXT_LINK_DOWNED_F, (uint64_t *)&acc->linkDownedCounter);
		mad_encode_field(pc, IB_PC_EXT_ERR_RCV_F, (uint64_t *)&acc->portRcvErrors);
		mad_encode_field(pc, IB_PC_EXT_ERR_PHYSRCV_F, (uint64_t *)&acc->portRcvRemotePhysicalErrors);
		mad_encode_field(pc, IB_PC_EXT_ERR_SWITCH_REL_F, (uint64_t *)&acc->portRcvSwitchRelayErrors);
		mad_encode_field(pc, IB_PC_EXT_XMT_DISCARDS_F, (uint64_t *)&acc->portXmitDiscards);
		mad_encode_field(pc, IB_PC_EXT_ERR_XMTCONSTR_F, (uint64_t *)&acc->portXmitConstraintErrors);
		mad_encode_field(pc, IB_PC_EXT_ERR_RCVCONSTR_F, (uint64_t *)&acc->portRcvConstraintErrors);
		mad_encode_field(pc, IB_PC_EXT_ERR_LOCALINTEG_F, (uint64_t *)&acc->localLinkIntegrityErrors);
		mad_encode_field(pc, IB_PC_EXT_ERR_EXCESS_OVR_F, (uint64_t *)&acc->excessiveBufferOverrunErrors);
		mad_encode_field(pc, IB_PC_EXT_VL15_DROPPED_F, (uint64_t *)&acc->VL15Dropped);
		mad_encode_field(pc, IB_PC_EXT_XMT_WAIT_F, (uint64_t *)&acc->portXmitWait);
		mad_encode_field(pc, IB_PC_EXT_QP1_DROP_F, (uint64_t *)&acc->QP1Dropped);
	}
}

// wrappers for dumps (always use IB_SMP_DATA_SIZE)
static void dump_perfcounters_ext(char *buf, int size, __be16 cap_mask, uint32_t cap_mask2, uint8_t *pc)
{
	size_t offset = 0, tmp_offset = 0;

	mad_dump_fields(buf, size, pc, IB_SMP_DATA_SIZE, IB_PC_EXT_FIRST_F, IB_PC_EXT_XMT_UPKTS_F);
	offset = strlen(buf);

	if (cap_mask & IB_PM_EXT_WIDTH_SUPPORTED) {
		mad_dump_fields(buf + offset, size - (int)offset, pc, IB_SMP_DATA_SIZE,
		                IB_PC_EXT_XMT_UPKTS_F, IB_PC_EXT_LAST_F);
		tmp_offset = strlen(buf + offset);
		offset += tmp_offset;
	}

	if (cap_mask2 & IB_PM_IS_ADDL_PORT_CTRS_EXT_SUP) {
		mad_dump_fields(buf + offset, size - (int)offset, pc, IB_SMP_DATA_SIZE,
		                IB_PC_EXT_COUNTER_SELECT2_F, IB_PC_EXT_ERR_LAST_F);
	}
}

static void write_output_to_file(const char *output, int size)
{
	pthread_mutex_lock(&output_mutex);
	if (output_file) {
		fwrite(output, 1, size, output_file);
		fflush(output_file);
	}
	pthread_mutex_unlock(&output_mutex);
}

// Thread function
static void *query_guid_thread(void *arg)
{
	thread_data_t *td = (thread_data_t *)arg;
	ib_portid_t portid = {0};
	uint8_t data[IB_SMP_DATA_SIZE] = {0};
	uint8_t pc[IB_SMP_DATA_SIZE] = {0};
	int node_type, num_ports = 0;
	int start_port = 1;
	int enhancedport0 = 0;
	int i;
	char buf[1536];
	char header[256];
	time_t timestamp = time(NULL);
	int attempts, ok;

	// resolve GUID (mutex because libibmad calls are not documented thread-safe)
	char guid_str[64];
    snprintf(guid_str, sizeof(guid_str), "%" PRIu64, td->guid);
    // snprintf(guid_str, sizeof(guid_str), "%016" PRIx64, td->guid);
    printf("GUID STRING %s\n", guid_str);

	ok = 0;
	for (attempts = 0; attempts < 3 && !ok; ++attempts) {
		pthread_mutex_lock(&rpc_mutex);
		int rc = resolve_portid_str(srcports->gsi.ca_name, ibd_ca_port, &portid,
		                            guid_str, IB_DEST_GUID, NULL, srcport);
		pthread_mutex_unlock(&rpc_mutex);
		ok = (rc >= 0);
		if (!ok) usleep(100000);
	}
	if (!ok) {
		td->output_size = snprintf(td->output_buffer, MAX_OUTPUT_SIZE,
			"# Thread %d: Failed to resolve GUID 0x%016" PRIx64 " at %s",
			td->thread_id, td->guid, ctime(&timestamp));
		return NULL;
	}

	// query NODE_INFO
	ok = 0;
	for (attempts = 0; attempts < 3 && !ok; ++attempts) {
		pthread_mutex_lock(&rpc_mutex);
		uint8_t *response_ptr = NULL; 
        response_ptr = smp_query_via(data, &portid, IB_ATTR_NODE_INFO, 0, 0, srcports->smi.port);
        if (!response_ptr) ok = 0;
        else ok = 1;
		pthread_mutex_unlock(&rpc_mutex);
		if (!ok) usleep(100000);
	}
	if (!ok) {
		td->output_size = snprintf(td->output_buffer, MAX_OUTPUT_SIZE,
			"# Thread %d: Failed to query node info for 0x%016" PRIx64 " at %s",
			td->thread_id, td->guid, ctime(&timestamp));
		return NULL;
	}

	node_type = mad_get_field(data, 0, IB_NODE_TYPE_F);
	mad_decode_field(data, IB_NODE_NPORTS_F, &num_ports);
	if (!num_ports) {
		td->output_size = snprintf(td->output_buffer, MAX_OUTPUT_SIZE,
			"# Thread %d: Invalid number of ports for 0x%016" PRIx64 " at %s",
			td->thread_id, td->guid, ctime(&timestamp));
		return NULL;
	}

	// SWITCH_INFO for enhanced port 0
	if (node_type == IB_NODE_SWITCH) {
		ok = 0;
		for (attempts = 0; attempts < 3 && !ok; ++attempts) {
			pthread_mutex_lock(&rpc_mutex);
           	uint8_t *response_ptr = NULL;
            response_ptr = smp_query_via(data, &portid, IB_ATTR_SWITCH_INFO, 0, 0, srcports->smi.port);
            if (!response_ptr) ok = 0;
            else ok = 1;

			pthread_mutex_unlock(&rpc_mutex);
			if (!ok) usleep(100000);
		}
		if (!ok) {
			td->output_size = snprintf(td->output_buffer, MAX_OUTPUT_SIZE,
				"# Thread %d: Failed to query switch info for 0x%016" PRIx64 " at %s",
				td->thread_id, td->guid, ctime(&timestamp));
			return NULL;
		}
		enhancedport0 = mad_get_field(data, 0, IB_SW_ENHANCED_PORT0_F);
		if (enhancedport0) start_port = 0;
	}

	// PM ClassPortInfo
	memset(pc, 0, IB_SMP_DATA_SIZE);
	ok = 0;
	for (attempts = 0; attempts < 3 && !ok; ++attempts) {
		pthread_mutex_lock(&rpc_mutex);
        uint8_t *response_ptr = NULL;
		response_ptr = pma_query_via(pc, &portid, 1, td->timeout, CLASS_PORT_INFO, srcport);
        if (!response_ptr) ok = 0;
        else ok = 1;

		pthread_mutex_unlock(&rpc_mutex);
		if (!ok) usleep(100000);
	}
	if (!ok) {
		td->output_size = snprintf(td->output_buffer, MAX_OUTPUT_SIZE,
			"# Thread %d: Failed to query class port info for 0x%016" PRIx64 " at %s",
			td->thread_id, td->guid, ctime(&timestamp));
		return NULL;
	}

	// Capability masks
	__be16 cap_mask_be;
	__be32 cap_mask2_be;
	uint32_t cap_mask2;
	memcpy(&cap_mask_be, pc + 2, sizeof(cap_mask_be));
	memcpy(&cap_mask2_be, pc + 4, sizeof(cap_mask2_be));
	__be16 cap_mask = cap_mask_be;
	cap_mask2 = (ntohl(cap_mask2_be) >> 5);

	// Header
	int off = 0;
	snprintf(header, sizeof(header),
		"# Thread %d: Querying GUID 0x%016" PRIx64 " with %d ports at %s",
		td->thread_id, td->guid, num_ports, ctime(&timestamp));
	strncpy(td->output_buffer, header, MAX_OUTPUT_SIZE - 1);
	td->output_buffer[MAX_OUTPUT_SIZE - 1] = '\0';
	off = (int)strlen(td->output_buffer);

	// Query each port
	for (i = start_port; i <= num_ports; i++) {
		char dump[1536] = {0};
		uint8_t pc_local[IB_SMP_DATA_SIZE] = {0};

		// query per-port counters
		if (td->extended != 1) {
			memset(pc_local, 0, IB_SMP_DATA_SIZE);
			ok = 0;
			for (attempts = 0; attempts < 3 && !ok; ++attempts) {
				pthread_mutex_lock(&rpc_mutex);
                uint8_t *response_ptr = NULL;
				response_ptr = pma_query_via(pc_local, &portid, i, td->timeout, IB_GSI_PORT_COUNTERS, srcport);
                if (!response_ptr) ok = 0;
                else ok = 1;


				pthread_mutex_unlock(&rpc_mutex);
				if (!ok) usleep(100000);
			}
			if (!ok) continue;

			// if not support XmitWait, clear
			if (!(cap_mask & IB_PM_PC_XMIT_WAIT_SUP)) {
				uint32_t zero = 0;
				mad_encode_field(pc_local, IB_PC_XMT_WAIT_F, &zero);
			}

			mad_dump_perfcounters(dump, sizeof(dump), pc_local, IB_SMP_DATA_SIZE);
		} else {
			// extended counters support?
			if (!((cap_mask & IB_PM_EXT_WIDTH_SUPPORTED) || (cap_mask & IB_PM_EXT_WIDTH_NOIETF_SUP)))
				continue;

			memset(pc_local, 0, IB_SMP_DATA_SIZE);
			ok = 0;
			for (attempts = 0; attempts < 3 && !ok; ++attempts) {
				pthread_mutex_lock(&rpc_mutex);
                uint8_t *response_ptr = NULL;
				response_ptr = pma_query_via(pc_local, &portid, i, td->timeout, IB_GSI_PORT_COUNTERS_EXT, srcport);
                if (!response_ptr) ok = 0;
                else ok = 1;
				pthread_mutex_unlock(&rpc_mutex);
				if (!ok) usleep(100000);
			}
			if (!ok) continue;

			dump_perfcounters_ext(dump, sizeof(dump), cap_mask, cap_mask2, pc_local);
		}

		if (dump[0] != '\0') {
			if (appendf(td->output_buffer, MAX_OUTPUT_SIZE, &off,
			            "# Port counters: %s port %d (CapMask: 0x%02X)\n%s",
			            portid2str(&portid), i, ntohs(cap_mask), dump) < 0) {
				// buffer full; stop appending further to avoid overflow
				break;
			}
		}
	}

	td->output_size = (int)strlen(td->output_buffer);
	return NULL;
}

// read GUIDs
static int read_guids_from_config(const char *config_file)
{
	FILE *file = fopen(config_file, "r");
	char line[MAX_LINE_LENGTH];
	int count = 0;
	if (!file) {
		fprintf(stderr, "Error: Cannot open config file %s\n", config_file);
		return -1;
	}
	while (fgets(line, sizeof(line), file) && count < MAX_GUIDS) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
		line[strcspn(line, "\n")] = 0;
		if (strncmp(line, "0x", 2) == 0) {
			guids[count++] = strtoull(line, NULL, 16);
		} else {
			guids[count++] = strtoull(line, NULL, 10);
		}
	}
	fclose(file);
	return count;
}

int main(int argc, char **argv)
{
	int mgmt_classes[3] = { IB_SMI_CLASS, IB_SA_CLASS, IB_PERFORMANCE_CLASS };
	const char *config_file = DEFAULT_CONFIG_FILE;
	const char *output_file_name = DEFAULT_OUTPUT_FILE;
	int extended = 0;
	int timeout = 20;
	int max_threads = 10;
	int verbose = 1;
	pthread_t *threads = NULL;
	thread_data_t *tds = NULL;
	int i, j;
	time_t start_time, end_time;

	// args
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) config_file = argv[++i];
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_file_name = argv[++i];
		else if (strcmp(argv[i], "-x") == 0) extended = 1;
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) timeout = atoi(argv[++i]);
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) max_threads = atoi(argv[++i]);
		else if (strcmp(argv[i], "-q") == 0) verbose = 0;
		else if (strcmp(argv[i], "-h") == 0) {
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -c <file>    Configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
			printf("  -o <file>    Output file (default: %s)\n", DEFAULT_OUTPUT_FILE);
			printf("  -x           Use extended counters\n");
			printf("  -t <timeout> Query timeout in seconds (default: 20)\n");
			printf("  -n <num>     Maximum number of threads (default: 10)\n");
			printf("  -q           Quiet mode - suppress MAD warnings\n");
			printf("  -h           Show this help\n");
			return 0;
		}
	}

	// config
	num_guids = read_guids_from_config(config_file);
	if (num_guids <= 0) {
		fprintf(stderr, "Error: No valid GUIDs found in config file %s\n", config_file);
		return 1;
	}
	printf("Found %d GUIDs in config file %s\n", num_guids, config_file);

	// output
	output_file = fopen(output_file_name, "w");
	if (!output_file) {
		fprintf(stderr, "Error: Cannot open output file %s\n", output_file_name);
		return 1;
	}

	// init MAD
	srcports = mad_rpc_open_port2(ibd_ca, ibd_ca_port, mgmt_classes, 3, 0);
	if (!srcports) {
		// SIMULATION MODE
		fprintf(stderr, "Warning: Failed to open '%s' port '%d' - running in simulation mode\n", ibd_ca, ibd_ca_port);
		fprintf(stderr, "This will generate sample output for testing purposes\n");

		start_time = time(NULL);
		fprintf(output_file, "# SIMULATION MODE - No IB devices available\n");
		fprintf(output_file, "# This is test output for %d GUIDs\n", num_guids);
		fprintf(output_file, "# Config file: %s\n", config_file);
		fprintf(output_file, "# Number of GUIDs: %d\n", num_guids);
		fprintf(output_file, "# Max threads: %d\n", max_threads);
		fprintf(output_file, "# Extended counters: %s\n", extended ? "yes" : "no");
		fprintf(output_file, "# Timeout: %d seconds\n", timeout);
		fprintf(output_file, "#\n");

		for (i = 0; i < num_guids; i++) {
			time_t ts = time(NULL);
			fprintf(output_file, "# Thread %d: Querying GUID 0x%016" PRIx64 " with 2 ports at %s",
			        i + 1, guids[i], ctime(&ts));
			fprintf(output_file, "# Port counters: 0x%016" PRIx64 " port 1 (CapMask: 0x02)\n", guids[i]);
			fprintf(output_file, "#\tPortXmitData: 0x00000000\n");
			fprintf(output_file, "#\tPortRcvData: 0x00000000\n");
			fprintf(output_file, "#\tPortXmitPkts: 0x00000000\n");
			fprintf(output_file, "#\tPortRcvPkts: 0x00000000\n");
			fprintf(output_file, "#\n");
		}
		end_time = time(NULL);
		fprintf(output_file, "#\n# Parallel perfquery completed at %s", ctime(&end_time));
		fprintf(output_file, "# Total time: %ld seconds\n", (long)(end_time - start_time));
		printf("Simulation completed. Results written to %s\n", output_file_name);
		fclose(output_file);
		return 0;
	}

	srcport = srcports->gsi.port;
	smp_mkey_set(srcports->smi.port, ibd_mkey);

	// alloc
	threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)max_threads);
	tds     = (thread_data_t *)malloc(sizeof(thread_data_t) * (size_t)num_guids);
	if (!threads || !tds) {
		fprintf(stderr, "Error: Memory allocation failed\n");
		if (threads) free(threads);
		if (tds) free(tds);
		mad_rpc_close_port2(srcports);
		fclose(output_file);
		return 1;
	}

	// header
	start_time = time(NULL);
	fprintf(output_file, "# Parallel perfquery started at %s", ctime(&start_time));
	fprintf(output_file, "# Config file: %s\n", config_file);
	fprintf(output_file, "# Number of GUIDs: %d\n", num_guids);
	fprintf(output_file, "# Max threads: %d\n", max_threads);
	fprintf(output_file, "# Extended counters: %s\n", extended ? "yes" : "no");
	fprintf(output_file, "# Timeout: %d seconds\n", timeout);
	fprintf(output_file, "#\n");

	// batches
	for (i = 0; i < num_guids; i += max_threads) {
		int batch_size = ((i + max_threads) <= num_guids) ? max_threads : (num_guids - i);
		int active = 0;

		printf("Processing batch %d-%d of %d GUIDs...\n", i + 1, i + batch_size, num_guids);

		for (j = 0; j < batch_size; j++) {
			tds[i + j].thread_id = i + j + 1;
			tds[i + j].guid      = guids[i + j];
			tds[i + j].extended  = extended;
			tds[i + j].timeout   = timeout;
			tds[i + j].verbose   = verbose;
			tds[i + j].output_size = 0;

			if (pthread_create(&threads[j], NULL, query_guid_thread, &tds[i + j]) != 0) {
				fprintf(stderr, "Error: Failed to create thread for GUID 0x%016" PRIx64 "\n", guids[i + j]);
				continue;
			}
			active++;
		}

		for (j = 0; j < active; j++) {
			pthread_join(threads[j], NULL);
		}

		for (j = 0; j < batch_size; j++) {
			if (tds[i + j].output_size > 0) {
				write_output_to_file(tds[i + j].output_buffer, tds[i + j].output_size);
			}
		}
	}

	// footer
	end_time = time(NULL);
	fprintf(output_file, "#\n# Parallel perfquery completed at %s", ctime(&end_time));
	fprintf(output_file, "# Total time: %ld seconds\n", (long)(end_time - start_time));

	printf("Parallel perfquery completed. Results written to %s\n", output_file_name);

	// cleanup
	free(threads);
	free(tds);
	mad_rpc_close_port2(srcports);
	fclose(output_file);
	return 0;
}

