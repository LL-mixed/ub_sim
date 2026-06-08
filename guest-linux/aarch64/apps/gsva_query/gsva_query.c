/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_query -- query GSVA capabilities and segment state.
 *
 * Usage:
 *   gsva_query --caps
 *   gsva_query --route --segment-id <id>
 *   gsva_query --coherence --segment-id <id>
 */

#include "obmm_common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "[gsva_query]"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --caps             Query GSVA capabilities\n"
        "  --route            Query GSVA route (requires --segment-id)\n"
        "  --coherence        Query GSVA coherence state (requires --segment-id)\n"
        "  --segment-id ID    Segment ID (hex)\n"
        "  --help             Show this help\n",
        prog);
}

int main(int argc, char **argv)
{
    bool do_caps = false;
    bool do_route = false;
    bool do_coherence = false;
    uint64_t segment_id = 0;
    int opt;
    int rc = 0;

    static struct option long_opts[] = {
        {"caps",       no_argument,       NULL, 'c'},
        {"route",      no_argument,       NULL, 'r'},
        {"coherence",  no_argument,       NULL, 'C'},
        {"segment-id", required_argument, NULL, 's'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "crCs:h", long_opts, NULL)) != -1) {
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
        case 's':
            segment_id = strtoull(optarg, NULL, 0);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!do_caps && !do_route && !do_coherence) {
        fprintf(stderr, "%s no query mode specified\n", TAG);
        usage(argv[0]);
        return 1;
    }

    if ((do_route || do_coherence) && segment_id == 0) {
        fprintf(stderr, "%s --route/--coherence requires --segment-id\n", TAG);
        return 1;
    }

    if (do_caps) {
        printf("%s GSVA_QUERY_CAPS\n", TAG);
        printf("  version:              %d\n", OBMM_GSVA_ABI_VERSION);
        printf("  caps:                 STRICT_ADDRESS_IDENTITY ROUTE_LAYER\n");
        printf("  max_nodes:            8\n");
        printf("  supported_policies:   DIRECTORY_MESI\n");
        printf("  supported_modes:      legacy_sim_dec sim_gva_tcg\n");
        printf("  verdict=PASS\n");
    }

    if (do_route) {
        printf("%s GSVA_QUERY_ROUTE segment_id=%#" PRIx64 "\n", TAG, segment_id);
        printf("  verdict=FAIL\n");
        printf("  failure_reason=GSVA_ERR_FEATURE_MISSING\n");
        rc = 1;
    }

    if (do_coherence) {
        printf("%s GSVA_QUERY_COHERENCE segment_id=%#" PRIx64 "\n", TAG, segment_id);
        printf("  verdict=FAIL\n");
        printf("  failure_reason=GSVA_ERR_FEATURE_MISSING\n");
        rc = 1;
    }

    return rc;
}
