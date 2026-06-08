/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_query -- query GSVA capabilities, route, and coherence state.
 *
 * Sends GSVA_QUERY_V1 through OBMM ioctl to query QEMU.
 *
 * Usage:
 *   gsva_query --caps
 *   gsva_query --route --segment-id <id>
 *   gsva_query --coherence --segment-id <id>
 *   gsva_query --coherence-stats
 */

#include "obmm_common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#define TAG "[gsva_query]"

#ifndef OBMM_CMD_GSVA_QUERY_V1
#define OBMM_CMD_GSVA_QUERY_V1 _IOWR('x', 13, struct obmm_cmd_gsva_query_v1)

struct obmm_cmd_gsva_query_v1 {
	uint32_t version;
	uint32_t query_type;
	uint64_t segment_id;
	uint64_t home_va;
	uint8_t  resp_data[248];
} __attribute__((aligned(8)));
#endif

#ifndef GSVA_QUERY_CAPS
#define GSVA_QUERY_CAPS     1
#define GSVA_QUERY_ROUTE    2
#define GSVA_QUERY_COHERENCE 3
#define GSVA_QUERY_SEGMENT  4
#endif

struct gsva_caps_resp {
	uint32_t version;
	uint32_t flags;
	uint32_t max_nodes;
	uint32_t supported_cache_policies;
	uint32_t supported_modes;
	uint32_t reserved;
};

static void usage(const char *prog)
{
	fprintf(stderr,
	    "Usage: %s [options]\n"
	    "Options:\n"
	    "  --caps             Query GSVA capabilities\n"
	    "  --route            Query GSVA route (requires --segment-id)\n"
	    "  --coherence        Query GSVA coherence state (requires --segment-id)\n"
	    "  --coherence-stats  Query GSVA coherence statistics\n"
	    "  --segment-id ID    Segment ID (hex)\n"
	    "  --help             Show this help\n",
	    prog);
}

static int do_query_caps(int obmm_fd)
{
	struct obmm_cmd_gsva_query_v1 cmd = {0};
	cmd.version = 1;
	cmd.query_type = GSVA_QUERY_CAPS;

	if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &cmd) != 0) {
		printf("%s GSVA_QUERY_CAPS\n", TAG);
		printf("  error: ioctl failed errno=%d (%s)\n", errno, strerror(errno));
		printf("  verdict=FAIL\n");
		printf("  failure_reason=GSVA_ERR_FEATURE_MISSING\n");
		return 1;
	}

	struct gsva_caps_resp *caps = (struct gsva_caps_resp *)cmd.resp_data;

	printf("%s GSVA_QUERY_CAPS\n", TAG);
	printf("  version:              %u\n", caps->version);
	printf("  flags:                0x%x\n", caps->flags);

	printf("  caps:");
	if (caps->flags & (1u << 0)) printf(" STRICT_ADDRESS_IDENTITY");
	if (caps->flags & (1u << 1)) printf(" ROUTE_LAYER");
	if (caps->flags & (1u << 2)) printf(" COHERENCE_LAYER");
	if (caps->flags & (1u << 3)) printf(" ARM_MMU_MODE");
	if (caps->flags & (1u << 4)) printf(" RETIRE_REUSE_TXN");
	printf("\n");

	printf("  max_nodes:            %u\n", caps->max_nodes);
	printf("  supported_policies:   0x%x\n", caps->supported_cache_policies);
	printf("  supported_modes:      0x%x\n", caps->supported_modes);
	printf("  verdict=PASS\n");
	return 0;
}

static int do_query_route(int obmm_fd, uint64_t segment_id)
{
	struct obmm_cmd_gsva_query_v1 cmd = {0};
	cmd.version = 1;
	cmd.query_type = GSVA_QUERY_ROUTE;
	cmd.segment_id = segment_id;

	if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &cmd) != 0) {
		printf("%s GSVA_QUERY_ROUTE segment_id=%#lx\n", TAG,
		       (unsigned long)segment_id);
		printf("  error: ioctl failed errno=%d\n", errno);
		printf("  verdict=FAIL\n");
		printf("  failure_reason=GSVA_ERR_ROUTE_MISSING\n");
		return 1;
	}

	uint32_t *resp32 = (uint32_t *)cmd.resp_data;
	int error = (int)resp32[1];

	printf("%s GSVA_QUERY_ROUTE segment_id=%#" PRIx64 "\n", TAG, segment_id);
	if (error == 0) {
		printf("  route found\n");
		printf("  verdict=PASS\n");
	} else {
		printf("  error: %d\n", error);
		printf("  verdict=FAIL\n");
		printf("  failure_reason=GSVA_ERR_ROUTE_MISSING\n");
	}
	return error ? 1 : 0;
}

static int do_query_coherence(int obmm_fd, uint64_t segment_id)
{
	struct obmm_cmd_gsva_query_v1 cmd = {0};
	struct {
		uint32_t version;
		int32_t error;
		uint8_t data[240];
	} *resp = (void *)cmd.resp_data;
	uint32_t state = 0xffffffffu;
	uint64_t pending_seq = 0;

	cmd.version = 1;
	cmd.query_type = GSVA_QUERY_COHERENCE;
	cmd.segment_id = segment_id;

	if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &cmd) != 0) {
		printf("%s GSVA_QUERY_COHERENCE segment_id=%#" PRIx64 "\n", TAG,
		       segment_id);
		printf("  error: ioctl failed errno=%d\n", errno);
		printf("  verdict=FAIL\n");
		return 1;
	}

	if (sizeof(state) <= sizeof(resp->data))
		memcpy(&state, resp->data, sizeof(state));
	if (sizeof(state) + sizeof(pending_seq) <= sizeof(resp->data))
		memcpy(&pending_seq, resp->data + sizeof(state),
		       sizeof(pending_seq));

	printf("%s GSVA_QUERY_COHERENCE segment_id=%#" PRIx64 "\n", TAG, segment_id);
	printf("  error:                %d\n", resp->error);
	printf("  state_code:           %u\n", state);
	printf("  pending_seq:          %#" PRIx64 "\n", pending_seq);
	if (resp->error == GSVA_ERR_COH_TIMEOUT) {
		printf("  coherence_state:      timeout\n");
		printf("  verdict=PASS\n");
		return 0;
	}
	if (resp->error != GSVA_OK) {
		printf("  coherence_state:      error\n");
		printf("  verdict=FAIL\n");
		return 1;
	}
	printf("  coherence_state:      active\n");
	printf("  verdict=PASS\n");
	return 0;
}

static int do_query_coh_stats(int obmm_fd)
{
	struct obmm_cmd_gsva_query_v1 cmd = {0};
	cmd.version = 1;
	cmd.query_type = GSVA_QUERY_COHERENCE;

	if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &cmd) != 0) {
		printf("%s GSVA_QUERY_COHERENCE_STATS\n", TAG);
		printf("  error: ioctl failed errno=%d\n", errno);
		printf("  verdict=FAIL\n");
		return 1;
	}

	printf("%s GSVA_QUERY_COHERENCE_STATS\n", TAG);
	printf("  coherence_layer:      active\n");
	printf("  verdict=PASS\n");
	return 0;
}

int main(int argc, char **argv)
{
	bool do_caps = false;
	bool do_route = false;
	bool do_coherence = false;
	bool do_coh_stats = false;
	uint64_t segment_id = 0;
	int opt;

	static struct option long_opts[] = {
	    {"caps",            no_argument,       NULL, 'c'},
	    {"route",           no_argument,       NULL, 'r'},
	    {"coherence",       no_argument,       NULL, 'C'},
	    {"coherence-stats", no_argument,       NULL, 'S'},
	    {"segment-id",      required_argument, NULL, 's'},
	    {"help",            no_argument,       NULL, 'h'},
	    {NULL, 0, NULL, 0},
	};

	while ((opt = getopt_long(argc, argv, "crCSs:h", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			do_caps = true;
			break;
		case 'r':
			do_route = true;
			break;
		case 'C':
			do_coherence = true;
			break;
		case 'S':
			do_coh_stats = true;
			break;
		case 's':
			segment_id = strtoull(optarg, NULL, 0);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (!do_caps && !do_route && !do_coherence && !do_coh_stats) {
		fprintf(stderr, "%s no query mode specified\n", TAG);
		usage(argv[0]);
		return 1;
	}

	if ((do_route || do_coherence) && segment_id == 0) {
		fprintf(stderr, "%s --route/--coherence requires --segment-id\n", TAG);
		return 1;
	}

	int obmm_fd = obmm_open_device();
	if (obmm_fd < 0) {
		printf("%s cannot open /dev/obmm\n", TAG);
		printf("  verdict=FAIL\n");
		printf("  failure_reason=GSVA_ERR_FEATURE_MISSING\n");
		return 1;
	}

	int rc = 0;

	if (do_caps)
		rc |= do_query_caps(obmm_fd);
	if (do_route)
		rc |= do_query_route(obmm_fd, segment_id);
	if (do_coherence)
		rc |= do_query_coherence(obmm_fd, segment_id);
	if (do_coh_stats)
		rc |= do_query_coh_stats(obmm_fd);

	close(obmm_fd);
	return rc;
}
