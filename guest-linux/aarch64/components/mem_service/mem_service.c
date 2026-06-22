#include "mem_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "kernel_ub/include/uapi/ub/obmm.h"
#include "common/obmm_common.h"
#include "mem_service_qwen3.h"
#include "libs/obmm_queue/obmm_queue_types.h"
#include "libs/obmm_queue/obmm_spsc_queue.h"

#define MEM_SERVICE_CLUSTER_MAX_NODES 8
#define MEM_SERVICE_CLUSTER_MAX_RECORDS 1024
#define MEM_SERVICE_MAYBE_UNUSED __attribute__((unused))
#define MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS 16ULL
#define MEM_SERVICE_DEFAULT_REGION_SIZE_MB 512
#define MEM_SERVICE_CMDLINE_REGION_SIZE "mem_service_region_size_mb"
#define MEM_SERVICE_CLUSTER_QUEUE_DEPTH 512
#define MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH 16
#define MEM_SERVICE_CLUSTER_WAIT_MS 300000L
#define MEM_SERVICE_OBMM_SERVICE_WAIT_MS 300000L
#define MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS 600000L
#define MEM_SERVICE_CLUSTER_IMPORT_ALIGN (2ULL * 1024ULL * 1024ULL)
#define MEM_SERVICE_CLUSTER_MAX_WINDOWS 16
#define MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES 8192ULL
#define MEM_SERVICE_OBMM_WEIGHT_OFFSET 0x10000ULL
#define MEM_SERVICE_OBMM_KVCACHE_OFFSET 0x14000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_INPUT_OFFSET 0x18000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_OUTPUT_OFFSET 0x58000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_RUNTIME_OUTPUT_OFFSET 0x98000ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET 0xda000ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS 1024ULL
#define MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_OFFSET 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES 0x200000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES 0x40000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES 0x80000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES \
    MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOTS 32ULL
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS 32ULL
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET \
    (MEM_SERVICE_OBMM_QWEN3_KV_STATE_OFFSET + \
     (MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES * \
      MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOTS))
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES \
    (MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS * \
     MEM_SERVICE_OBMM_HIDDEN_RANGE_BYTES)
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_BASE_OFFSET \
    (MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET + \
     MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES)
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_SLOT_BYTES 0x4000ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_HISTORY_BYTES 0x2000ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES 256ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES 64ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES 128ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_BYTES 262144ULL
#ifndef MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES
#define MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES 64ULL
#endif
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES 64ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_REGION_BYTES \
    (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS * \
     MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES)
#define MEM_SERVICE_OBMM_KIND_WEIGHT_TILE 1U
#define MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK 2U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT 3U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT 4U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT 5U
#ifndef MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT
#define MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT 6U
#endif
#define MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE 7U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY 8U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES 9U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED 10U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE 11U

#ifndef major
#define major(dev) ((unsigned int)(((uint64_t)(dev) >> 24) & 0xffU))
#endif
#ifndef minor
#define minor(dev) ((unsigned int)((uint64_t)(dev) & 0xffffffU))
#endif

struct mem_service_cluster_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct mem_service_cluster_payload {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
    uint8_t record_pad[48];
    struct mem_service_record records[MEM_SERVICE_CLUSTER_MAX_RECORDS];
};

static long mem_service_env_wait_ms_or_default(const char *name, long fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > (unsigned long long)LONG_MAX) {
        return fallback;
    }
    return (long)parsed;
}

static const char *mem_service_run_id_from_env(void)
{
    const char *run_id = getenv("MEM_SERVICE_RUN_ID");

    if (run_id && run_id[0] != '\0') {
        return run_id;
    }
    run_id = getenv("SIM_W5_RUN_ID");
    return run_id && run_id[0] != '\0' ? run_id : NULL;
}

static long mem_service_qwen3_runtime_range_wait_ms(void)
{
    long barrier_wait_ms;
    long runtime_wait_ms =
        mem_service_env_wait_ms_or_default("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", -1);

    if (runtime_wait_ms > 0) {
        return runtime_wait_ms;
    }
    barrier_wait_ms = mem_service_env_wait_ms_or_default(
        "SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS",
        MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS);
    return barrier_wait_ms > 0 ? barrier_wait_ms :
        MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS;
}

struct mem_service_cluster_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
};

struct mem_service_cluster_payload_compact_summary {
    uint16_t record_count;
    uint16_t prefix_count;
    uint16_t block_count;
    uint16_t group_count;
    uint16_t weight_tile_count;
    uint16_t kvcache_object_count;
    uint16_t flags;
    uint16_t hidden_range_count;
    uint64_t block_version_floor;
    uint64_t block_result_floor;
    uint64_t prefix_version_floor;
    uint64_t prefix_result_floor;
};

struct mem_service_qwen3_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

#define MEM_SERVICE_COMPACT_PREFIX_STATE_READY 0x0001U
#define MEM_SERVICE_COMPACT_PREFIX_VIEW_READY 0x0002U

struct mem_service_mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct mem_service_cluster_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint32_t export_cna;
    uint64_t mem_id;
    uint64_t local_pa;
    struct mem_service_mapped_region region;
};

struct mem_service_cluster_runtime {
    bool active;
    bool lazy_remote_activation;
    int node_count;
    int local_idx;
    int obmm_fd;
    uint32_t local_cna;
    uint32_t publish_seq;
    uint16_t observe_epoch;
    uint64_t region_size;
    uint64_t payload_offset;
    uint64_t payload_arena_base;
    uint64_t payload_arena_next;
    uint64_t payload_arena_high_water;
    bool pool_layout_reported;
    struct mem_service_cluster_meta metas[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct mem_service_cluster_slot slots[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_spsc_queue *ingress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    void *ingress_queue_base;
    struct obmm_spsc_queue *egress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_helpers_region egress_import[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_desc pending_descs[MEM_SERVICE_CLUSTER_MAX_NODES][MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH];
    uint8_t pending_desc_count[MEM_SERVICE_CLUSTER_MAX_NODES];
};

#define MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC 0x57344450U
#define MEM_SERVICE_CLUSTER_PAYLOAD_VERSION 1U

static struct mem_service_cluster_runtime g_mem_service_cluster_runtime;

static struct mem_service_record *mem_service_alloc_record(struct mem_service *svc);
static struct mem_service_record *mem_service_find_record(struct mem_service *svc, const char *key);
static struct mem_service_record *mem_service_recycle_qwen3_runtime_record(
    struct mem_service *svc,
    const char *incoming_key);
static int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);

#include "mem_service_cluster_utils.inc"

#include "mem_service_cluster_payload.inc"

#include "mem_service_object_refs.inc"

#include "mem_service_obmm_objects.inc"

#include "mem_service_qwen3_runtime.inc"

#include "mem_service_cluster_read.inc"

#include "mem_service_cluster_runtime.inc"

#include "mem_service_cluster_queue.inc"

#include "mem_service_records.inc"
#include "mem_service_qwen3_records.inc"
#include "mem_service_keys.inc"
#include "mem_service_metadata.inc"

#include "mem_service_cluster_observe.inc"

#include "mem_service_obmm_object_flow.inc"

static int mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    bool allow_terminal_commit,
    struct mem_service_object_payload_view *view_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_qwen3_layer_range_placement source_placement;
    struct mem_service_cluster_slot *source_slot = NULL;
    struct mem_service_record remote_hidden_output;
    struct obmm_desc handoff_desc;
    struct obmm_desc terminal_desc;
    char ingress_key[96];
    char token_result_key[96];
    long deadline;
    long wait_enter_ms;
    long found_local_ms = 0;
    long found_ms = 0;
    long checksum_ms;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    long producer_to_found_ms = 0;
    long producer_to_found_monotonic_ms = 0;
    uint64_t activate_ms = 0;
    uint64_t metadata_ms = 0;
    uint32_t attempts = 0;
    uint16_t expected_epoch;
    unsigned int relax_attempt = 0;
    uint32_t source_node = UINT32_MAX;
    uint32_t terminal_source_node = UINT32_MAX;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    const uint8_t *payload_view;
    uint64_t checksum;
    bool terminal_desc_found = false;

    if (!view_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;
    if (local_placement.layer_start == 0 && decode_step == 0) {
        return 0;
    }
    if (local_placement.layer_start == 0) {
        struct mem_service_record token_record;
        struct mem_service_cluster_slot *owner_slot;
        char token_result_key[96];
        struct obmm_desc token_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        bool token_input_found = false;
        bool token_desc_found = false;

        if (mem_service_cluster_runtime_require(rt) != 0) {
            return -1;
        }
        memset(&token_desc, 0, sizeof(token_desc));
        wait_enter_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        token_result_key[0] = '\0';
        expected_epoch = (uint16_t)(decode_step & 0xffffU);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
        while (obmm_now_ms() < deadline) {
            int owner_idx;

            attempts++;
            for (owner_idx = 0;
                 !token_desc_found && owner_idx < rt->node_count;
                 ++owner_idx) {
                struct obmm_desc rx;

                if (mem_service_take_pending_qwen3_token_result_desc(rt,
                                                               owner_idx,
                                                               expected_epoch,
                                                               &token_desc)) {
                    source_node = (uint32_t)owner_idx;
                    token_desc_found = true;
                    break;
                }
                if (owner_idx == rt->local_idx ||
                    !rt->ingress_queues[owner_idx]) {
                    continue;
                }
                while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                    if (mem_service_qwen3_token_result_desc_matches(&rx,
                                                              expected_epoch)) {
                        token_desc = rx;
                        source_node = (uint32_t)owner_idx;
                        token_desc_found = true;
                        break;
                    }
                    mem_service_stash_pending_desc(rt, owner_idx, &rx);
                }
            }
            if (!token_desc_found) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            if (!rt->slots[source_node].region.addr) {
                long activate_start_ms = obmm_now_ms();

                if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
            }
            owner_slot = &rt->slots[source_node];
            memset(&token_record, 0, sizeof(token_record));
            {
                long metadata_start_ms = obmm_now_ms();
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!owner_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(owner_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record_by_obmm_object_backing(
                        owner_slot,
                        MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                        token_desc.payload_offset,
                        token_desc.payload_len,
                        token_desc.cookie,
                        &token_record)) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
            }
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "%s",
                     token_record.key);
            if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                token_record.object_backing_offset != token_desc.payload_offset ||
                token_record.object_backing_len != token_desc.payload_len ||
                token_desc.cookie !=
                    (uint32_t)(token_record.object_payload_checksum ^
                               (token_record.object_payload_checksum >> 32)) ||
                token_record.object_backing_offset > owner_slot->region.len ||
                token_record.object_backing_len >
                    owner_slot->region.len - token_record.object_backing_offset) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_invalid local=node%u source=node%u key=%s\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key);
                return -1;
            }
            payload_view =
                (const uint8_t *)owner_slot->region.addr +
                token_record.object_backing_offset;
            memcpy(payload_words,
                   payload_view,
                   MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (payload_words[0] != decode_step - 1U) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_step_mismatch local=node%u source=node%u key=%s got=%" PRIu64 " expected=%" PRIu64 "\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key,
                       payload_words[0],
                       decode_step - 1U);
                return -1;
            }
            checksum = mem_service_qwen3_hidden_payload_checksum(
                payload_view,
                MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (checksum != token_record.object_payload_checksum) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            token_input_found = true;
            break;
        }
        if (!token_input_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_token_input_wait_failed local=node%u source=%s step=%" PRIu64 " epoch=%u attempts=%u desc_found=%u\n",
                   local_node + 1U,
                   source_node == UINT32_MAX ? "none" : "descriptor",
                   decode_step - 1U,
                   expected_epoch,
                   attempts,
                   token_desc_found ? 1U : 0U);
            return -1;
        }
        found_local_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        producer_publish_ms = (long)token_record.last_result_segment;
        producer_publish_monotonic_ms =
            (long)token_record.object_publish_monotonic_ms;
        producer_clock_offset_ms =
            (long)token_record.object_publish_supernode_offset_ms;
        if (producer_publish_ms > 0 && found_ms > 0) {
            producer_to_found_ms = found_ms - producer_publish_ms;
        }
        if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
            producer_to_found_monotonic_ms =
                found_local_ms - producer_publish_monotonic_ms;
        }
        memset(view_out, 0, sizeof(*view_out));
        view_out->data = payload_view;
        view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
        view_out->checksum = checksum;
        view_out->owner_node = source_node;
        view_out->payload_kind = token_record.object_payload_kind;
        view_out->backing_offset = token_record.object_backing_offset;
        memcpy(view_out->token_result_words,
               payload_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                            &view_out->object_ref) != 0) {
            return -1;
        }
        view_out->wait_enter_monotonic_ms =
            wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
        view_out->found_monotonic_ms =
            found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
        view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
        view_out->producer_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        view_out->producer_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        view_out->producer_clock_offset_ms = producer_clock_offset_ms;
        view_out->producer_to_found_supernode_ms = producer_to_found_ms;
        view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
        view_out->source_node = source_node;
        view_out->wait_attempts = attempts;
        view_out->activate_ms = activate_ms;
        view_out->metadata_ms = metadata_ms;
        printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " token=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=0 validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem source=terminal_token_result target=mapped_view status=ok\n",
               local_node + 1U,
               source_node + 1U,
               token_result_key,
               view_out->object_ref.key_hash,
               view_out->object_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               checksum,
               view_out->len,
               payload_words[1],
               found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               producer_to_found_ms,
               producer_to_found_monotonic_ms,
               view_out->wait_attempts,
               activate_ms,
               metadata_ms);
        return 0;
    }
    memset(&source_placement, 0, sizeof(source_placement));
    source_placement.owner_node = local_node - 1U;
    source_placement.next_owner_node = local_node;
    source_placement.terminal = false;
    if (mem_service_qwen3_layer_range_for_node(source_placement.owner_node,
                                         cluster_node_count,
                                         &source_placement.layer_start,
                                         &source_placement.layer_end,
                                         &source_placement.next_owner_node) != 0 ||
        source_placement.next_owner_node != local_node) {
        return -1;
    }
    source_placement.layer_count =
        source_placement.layer_end - source_placement.layer_start;
    source_node = source_placement.owner_node;
    if (mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (source_node >= cluster_node_count || !rt->ingress_queues[source_node]) {
        return -1;
    }
    ingress_key[0] = '\0';

    memset(&remote_hidden_output, 0, sizeof(remote_hidden_output));
    memset(&handoff_desc, 0, sizeof(handoff_desc));
    memset(&terminal_desc, 0, sizeof(terminal_desc));
    expected_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (expected_epoch == 0) {
        expected_epoch = 1;
    }
    wait_enter_ms = obmm_now_ms();
    deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
    while (obmm_now_ms() < deadline) {
        struct obmm_desc rx;

        attempts++;
        if (allow_terminal_commit) {
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "tokens/%s/decode-step%" PRIu64,
                     mem_service_qwen3_model_key(),
                     decode_step);
            if (!terminal_desc_found) {
                for (int owner_idx = 0;
                     !terminal_desc_found && owner_idx < rt->node_count;
                     ++owner_idx) {
                    struct obmm_desc rx;

                    if (mem_service_take_pending_qwen3_token_result_desc(
                            rt,
                            owner_idx,
                            expected_epoch,
                            &terminal_desc)) {
                        terminal_source_node = (uint32_t)owner_idx;
                        terminal_desc_found = true;
                        break;
                    }
                    if (owner_idx == rt->local_idx ||
                        !rt->ingress_queues[owner_idx]) {
                        continue;
                    }
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_token_result_desc_matches(
                                &rx,
                                expected_epoch)) {
                            terminal_desc = rx;
                            terminal_source_node = (uint32_t)owner_idx;
                            terminal_desc_found = true;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
            }
            if (terminal_desc_found &&
                terminal_source_node < (uint32_t)rt->node_count) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (terminal_source_node != (uint32_t)rt->local_idx &&
                    !rt->slots[terminal_source_node].region.addr) {
                    long activate_start_ms = obmm_now_ms();

                    if (mem_service_activate_remote_slot(
                            rt,
                            (int)terminal_source_node) != 0) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    activate_ms +=
                        (uint64_t)(obmm_now_ms() - activate_start_ms);
                }
                token_slot = &rt->slots[terminal_source_node];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                {
                    long metadata_start_ms = obmm_now_ms();

                    if (!token_slot->region.addr ||
                        !mem_service_try_read_stable_compact_summary_region(
                            token_slot,
                            &compact,
                            &seen) ||
                        !mem_service_slot_find_record_by_obmm_object_backing(
                            token_slot,
                            MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                            MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                            terminal_desc.payload_offset,
                            terminal_desc.payload_len,
                            terminal_desc.cookie,
                            &token_record)) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    metadata_ms +=
                        (uint64_t)(obmm_now_ms() - metadata_start_ms);
                }
                if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind !=
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len !=
                        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset !=
                        terminal_desc.payload_offset ||
                    token_record.object_backing_len != terminal_desc.payload_len ||
                    terminal_desc.cookie !=
                        (uint32_t)(token_record.object_payload_checksum ^
                                   (token_record.object_payload_checksum >> 32)) ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len -
                            token_record.object_backing_offset) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = terminal_source_node;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = terminal_source_node;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%u step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result"
                       " target=decode_round_scheduler receive=descriptor"
                       " status=committed\n",
                       local_node + 1U,
                       terminal_source_node + 1U,
                       decode_step,
                       payload_words[1],
                       token_record.key,
                       token_checksum);
                return 0;
            }
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (owner_idx != rt->local_idx &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                token_slot = &rt->slots[owner_idx];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!token_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(token_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record(token_slot,
                                            token_result_key,
                                            &token_record) ||
                    token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len - token_record.object_backing_offset) {
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = (uint32_t)owner_idx;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = (uint32_t)owner_idx;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%d step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result target=decode_round_scheduler"
                       " status=committed\n",
                       local_node + 1U,
                       owner_idx + 1,
                       decode_step,
                       payload_words[1],
                       token_result_key,
                       token_checksum);
                return 0;
            }
        }
        if (mem_service_take_pending_runtime_range_input_desc(rt,
                                                        (int)source_node,
                                                        expected_epoch,
                                                        &handoff_desc)) {
            break;
        }
        while (obmm_spsc_pop(rt->ingress_queues[source_node], &rx) == 0) {
            if (mem_service_runtime_range_input_desc_matches(&rx,
                                                       expected_epoch)) {
                handoff_desc = rx;
                break;
            }
            mem_service_stash_pending_desc(rt, (int)source_node, &rx);
        }
        if (handoff_desc.type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
            break;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    if (!mem_service_runtime_range_input_desc_matches(&handoff_desc,
                                                expected_epoch)) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_desc_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               attempts);
        return -1;
    }
    found_local_ms = obmm_now_ms();
    found_ms = mem_service_wallclock_ms();
    if (!rt->slots[source_node].region.addr) {
        long activate_start_ms = obmm_now_ms();

        if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
            return -1;
        }
        activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
    }
    {
        bool metadata_found = false;

        source_slot = &rt->slots[source_node];
        while (obmm_now_ms() < deadline) {
            long metadata_start_ms = obmm_now_ms();
            struct mem_service_cluster_payload_compact_summary compact;
            struct mem_service_cluster_payload_header seen;

            memset(&compact, 0, sizeof(compact));
            memset(&seen, 0, sizeof(seen));
            if (mem_service_try_read_stable_compact_summary_region(source_slot,
                                                             &compact,
                                                             &seen) &&
                mem_service_slot_find_record_by_obmm_object_backing(
                    source_slot,
                    MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                    MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                    handoff_desc.payload_offset,
                    handoff_desc.payload_len,
                    handoff_desc.cookie,
                    &remote_hidden_output)) {
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
                metadata_found = true;
                break;
            }
            mem_service_cpu_relax_wait(&relax_attempt);
        }
        if (!metadata_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_ingress_metadata_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
                   local_node + 1U,
                   source_node + 1U,
                   decode_step,
                   attempts,
                   handoff_desc.payload_offset,
                   (uint64_t)handoff_desc.payload_len);
            return -1;
        }
    }
    snprintf(ingress_key,
             sizeof(ingress_key),
             "%s",
             remote_hidden_output.key);
    if (!source_slot ||
        remote_hidden_output.object_payload_kind !=
            MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        remote_hidden_output.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_offset != handoff_desc.payload_offset ||
        remote_hidden_output.object_backing_len != handoff_desc.payload_len ||
        handoff_desc.cookie !=
            (uint32_t)(remote_hidden_output.object_payload_checksum ^
                       (remote_hidden_output.object_payload_checksum >> 32)) ||
        !source_slot->region.addr ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len >
            source_slot->region.len) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_wait_failed local=node%u source=node%u step=%" PRIu64 " key=%s\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               ingress_key);
        return -1;
    }
    payload_view =
        (const uint8_t *)source_slot->region.addr +
        remote_hidden_output.object_backing_offset;
    checksum = remote_hidden_output.object_payload_checksum;
    checksum_ms = 0;
    producer_publish_ms = (long)remote_hidden_output.last_result_segment;
    producer_publish_monotonic_ms =
        (long)remote_hidden_output.object_publish_monotonic_ms;
    producer_clock_offset_ms =
        (long)remote_hidden_output.object_publish_supernode_offset_ms;
    if (producer_publish_ms > 0 && found_ms > 0) {
        producer_to_found_ms = found_ms - producer_publish_ms;
    }
    if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
        producer_to_found_monotonic_ms =
            found_local_ms - producer_publish_monotonic_ms;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->data = payload_view;
    view_out->len = hidden_range_bytes;
    view_out->checksum = checksum;
    view_out->owner_node = source_node;
    view_out->payload_kind = remote_hidden_output.object_payload_kind;
    view_out->backing_offset = remote_hidden_output.object_backing_offset;
    if (mem_service_record_to_lingqu_obmm_ref(&remote_hidden_output,
                                        &view_out->object_ref) != 0) {
        return -1;
    }
    view_out->wait_enter_monotonic_ms =
        wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
    view_out->found_monotonic_ms =
        found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
    view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
    view_out->producer_publish_supernode_ms =
        producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
    view_out->producer_publish_monotonic_ms =
        producer_publish_monotonic_ms > 0 ?
            (uint64_t)producer_publish_monotonic_ms :
            0;
    view_out->producer_clock_offset_ms = producer_clock_offset_ms;
    view_out->producer_to_found_supernode_ms = producer_to_found_ms;
    view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
    view_out->source_node = source_node;
    view_out->wait_attempts = attempts;
    view_out->activate_ms = activate_ms;
    view_out->metadata_ms = metadata_ms;
    printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=%ld validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem target=mapped_view status=ok\n",
           local_node + 1U,
           source_node + 1U,
           ingress_key,
           view_out->object_ref.key_hash,
           view_out->object_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           checksum,
           hidden_range_bytes,
           found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           producer_to_found_ms,
           producer_to_found_monotonic_ms,
           attempts,
           activate_ms,
           metadata_ms,
           checksum_ms);
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input_view(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out)
{
    return mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
        local_node,
        cluster_node_count,
        decode_step,
        false,
        view_out);
}

int mem_service_obmm_service_v0_wait_scheduler_work_item(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_scheduler_work_item *item_out)
{
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_object_payload_view view;

    if (!item_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(item_out, 0, sizeof(*item_out));
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;

    if (local_placement.layer_start == 0 && decode_step == 0) {
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
        return 0;
    }

    memset(&view, 0, sizeof(view));
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
            local_node,
            cluster_node_count,
            decode_step,
            local_placement.layer_start > 0,
            &view) != 0 ||
        !view.data) {
        return -1;
    }
    if (view.payload_kind == MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT &&
        view.len == MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES) {
        uint64_t token_words[8];

        if (local_placement.layer_start == 0) {
            item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
            item_out->range_input = view;
            return 0;
        }
        memcpy(token_words,
               view.token_result_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (token_words[0] != decode_step) {
            printf("[mem_service] gap qwen3_scheduler_work_item=terminal_step_mismatch local=node%u got=%" PRIu64 " expected=%" PRIu64 "\n",
                   local_node + 1U,
                   token_words[0],
                   decode_step);
            return -1;
        }
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_NO_DISPATCH;
        item_out->terminal_step = token_words[0];
        item_out->terminal_token = token_words[1];
        item_out->terminal_owner_node = view.source_node;
        item_out->checksum = view.checksum;
        item_out->wait_enter_monotonic_ms = view.wait_enter_monotonic_ms;
        item_out->found_monotonic_ms = view.found_monotonic_ms;
        item_out->ready_monotonic_ms = view.ready_monotonic_ms;
        item_out->producer_publish_supernode_ms =
            view.producer_publish_supernode_ms;
        item_out->producer_publish_monotonic_ms =
            view.producer_publish_monotonic_ms;
        item_out->producer_clock_offset_ms = view.producer_clock_offset_ms;
        item_out->producer_to_found_supernode_ms =
            view.producer_to_found_supernode_ms;
        item_out->producer_to_found_monotonic_ms =
            view.producer_to_found_monotonic_ms;
        item_out->wait_attempts = view.wait_attempts;
        item_out->activate_ms = view.activate_ms;
        item_out->metadata_ms = view.metadata_ms;
        return 0;
    }
    if (view.payload_kind != MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT) {
        printf("[mem_service] gap qwen3_scheduler_work_item=input_kind_invalid local=node%u kind=%u bytes=%" PRIu64 "\n",
               local_node + 1U,
               view.payload_kind,
               view.len);
        return -1;
    }
    item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
    item_out->range_input = view;
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint64_t decode_step,
                                                   uint8_t *payload_out,
                                                   uint64_t payload_len,
                                                   uint64_t *checksum_out)
{
    struct mem_service_object_payload_view view;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);

    if (!payload_out || payload_len != hidden_range_bytes || !checksum_out) {
        return -1;
    }
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view(local_node,
                                                            cluster_node_count,
                                                            decode_step,
                                                            &view) != 0 ||
        !view.data ||
        view.len != payload_len) {
        return -1;
    }
    memcpy(payload_out, view.data, payload_len);
    *checksum_out = view.checksum;
    return 0;
}

int mem_service_obmm_service_v0_publish_runtime_range_output(struct mem_service *svc,
                                                       uint32_t local_node,
                                                       uint32_t cluster_node_count,
                                                       uint64_t decode_step,
                                                       const uint8_t *payload,
                                                       uint64_t payload_len,
                                                       uint64_t expected_checksum,
                                                       const uint8_t *kv_payload,
                                                       uint64_t kv_payload_len,
                                                       uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_hidden_output;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    uint32_t target_node;
    bool terminal_range;
    char local_hidden_output_key[96];
    char local_kv_state_key[96];
    uint64_t checksum;
    uint64_t kv_checksum = 0;
    uint64_t kv_state_offset = 0;
    uint64_t kv_state_block_bytes = 0;
    uint64_t kv_state_block_count = 0;
    uint64_t kv_state_reserved_bytes = 0;
    uint64_t runtime_output_offset = 0;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    struct lingqu_obmm_object_ref_wire hidden_ref;
    struct lingqu_obmm_object_ref_wire kv_ref;

    if (!svc || !payload || payload_len != hidden_range_bytes ||
        !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    checksum = mem_service_qwen3_hidden_payload_checksum(payload, payload_len);
    if (checksum != expected_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_output_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
               local_node + 1U,
               checksum,
               expected_checksum);
        return -1;
    }
    kv_checksum = mem_service_qwen3_hidden_payload_checksum(kv_payload, kv_payload_len);
    if (kv_checksum != expected_kv_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
               local_node + 1U,
               kv_checksum,
               expected_kv_checksum,
               kv_payload_len);
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    terminal_range = local_placement.layer_end >= mem_service_qwen3_layer_count();
    target_node = terminal_range ? local_node : local_placement.next_owner_node;
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  payload_len,
                                  64,
                                  &runtime_output_offset) != 0) {
        return -1;
    }
    if (mem_service_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_block_span_alloc_failed local=node%u step=%" PRIu64 " bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " region_len=%zu\n",
               local_node + 1U,
               decode_step,
               kv_payload_len,
               kv_state_block_bytes,
               kv_state_block_count,
               kv_state_reserved_bytes,
               local_slot->region.len);
        return -1;
    }
    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + runtime_output_offset, payload, payload_len);
    memcpy(base + kv_state_offset, kv_payload, kv_payload_len);
    if (mem_service_update_region_range_at(local_slot,
                                     runtime_output_offset,
                                     payload_len,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     kv_state_offset,
                                     kv_payload_len,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + runtime_output_offset,
                payload_len,
                MS_SYNC);
    (void)msync(base + kv_state_offset, kv_payload_len, MS_SYNC);
    snprintf(local_hidden_output_key,
             sizeof(local_hidden_output_key),
             terminal_range ?
                 "hidden/%s/node%u/range-runtime-output/decode-step%" PRIu64 :
                 "hidden/%s/node%u/range-runtime-input/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             target_node + 1U,
             decode_step);
    snprintf(local_kv_state_key,
             sizeof(local_kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     terminal_range ?
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT :
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                     runtime_output_offset,
                                     payload_len,
                                     checksum,
                                     &local_hidden_output) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_KVCACHE_OBJECT,
                                     local_kv_state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
                                     kv_state_offset,
                                     kv_payload_len,
                                     kv_checksum,
                                     &local_kv_state) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_hidden_output_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_hidden_output.last_result_segment = (uint64_t)producer_publish_ms;
        local_hidden_output.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_hidden_output.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_hidden_output.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_kv_state_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_kv_state.last_result_segment = (uint64_t)producer_publish_ms;
        local_kv_state.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_kv_state.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_kv_state.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    if (mem_service_record_to_lingqu_obmm_ref(&local_hidden_output, &hidden_ref) != 0 ||
        mem_service_record_to_lingqu_obmm_ref(&local_kv_state, &kv_ref) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    char boundary_observation_id[384];
    const char *service_run_id = mem_service_run_id_from_env();

    boundary_observation_id[0] = '\0';
    if (service_run_id) {
        snprintf(boundary_observation_id,
                 sizeof(boundary_observation_id),
                 "boundary-observation/%s/step%" PRIu64 "/node%u",
                 service_run_id,
                 decode_step,
                 local_node + 1U);
    }
    if (!terminal_range &&
        mem_service_push_obmm_object_desc_to(rt,
                                       target_node,
                                       MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                       local_hidden_output.object_backing_offset,
                                       local_hidden_output.object_backing_len,
                                       local_hidden_output.object_payload_checksum,
                                       object_epoch) != 0) {
        return -1;
    }
    if (!terminal_range && boundary_observation_id[0] != '\0') {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u observation_id=%s step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               boundary_observation_id,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    } else if (!terminal_range) {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    }
    printf("[mem_service] stage qwen3_range_forward_runtime_output_publish local=node%u step=%" PRIu64 " key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u output_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           hidden_ref.key_hash,
           hidden_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           checksum,
           payload_len,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           object_epoch,
           local_publish_seq);
    printf("[mem_service] stage qwen3_range_kv_state_publish local=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " slot_bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " producer_publish_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service status=ok\n",
           local_node + 1U,
           decode_step,
           local_kv_state_key,
           kv_ref.key_hash,
           kv_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_payload_len,
           kv_checksum,
           kv_state_offset,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES,
           kv_state_block_bytes,
           kv_state_block_count,
           kv_state_reserved_bytes,
           producer_publish_ms,
           object_epoch,
           local_publish_seq);
    return 0;
}

#include "mem_service_qwen3_kv_state_flow.inc"

#include "mem_service_qwen3_terminal_token_flow.inc"

#include "mem_service_qwen3_engram_publish_flow.inc"

int mem_service_obmm_service_v0_wait_engram_candidates(struct mem_service *svc,
                                                 uint64_t decode_step,
                                                 uint64_t timeout_ms,
                                                 uint64_t *candidate_tokens_out,
                                                 uint64_t *candidate_logit_bits_out,
                                                 uint64_t *candidate_text_checksums_out,
                                                 uint64_t *candidate_piece_bytes_out,
                                                 uint64_t *candidate_piece_word0_out,
                                                 uint64_t *candidate_piece_word1_out,
                                                 uint64_t candidate_capacity,
                                                 uint64_t *candidate_count_out,
                                                 uint64_t *candidate_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char candidates_key[96];
    long deadline;
    int candidate_owner_idx = -1;

    if (!svc || !candidate_tokens_out || !candidate_count_out ||
        candidate_capacity == 0) {
        return -1;
    }
    *candidate_count_out = 0;
    if (candidate_checksum_out) {
        *candidate_checksum_out = 0;
    }
    mem_service_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record candidates_record;
        struct obmm_desc candidates_desc;
        uint64_t candidate_words[32];
        uint64_t candidate_count;
        uint64_t inner_checksum;
        uint64_t candidates_checksum;
        bool candidates_record_found = false;

        candidate_owner_idx = -1;
        memset(&candidates_record, 0, sizeof(candidates_record));
        memset(&candidates_desc, 0, sizeof(candidates_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            rt->node_count > 0) {
            for (int node_idx = 0; node_idx < rt->node_count; ++node_idx) {
                terminal_slot = &rt->slots[node_idx];
                memset(&candidates_record, 0, sizeof(candidates_record));
                if (node_idx == rt->local_idx) {
                    candidates_record_found =
                        mem_service_slot_find_record(terminal_slot,
                                               candidates_key,
                                               &candidates_record);
                } else {
                    struct obmm_desc rx;
                    struct mem_service_cluster_payload_compact_summary compact;
                    struct mem_service_cluster_payload_header seen;

                    if (mem_service_take_pending_qwen3_object_kind_len_desc(
                            rt,
                            node_idx,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            &candidates_desc)) {
                    } else if (rt->ingress_queues[node_idx]) {
                        while (obmm_spsc_pop(rt->ingress_queues[node_idx], &rx) == 0) {
                            if (mem_service_qwen3_object_desc_kind_len_matches(
                                    &rx,
                                    MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES)) {
                                candidates_desc = rx;
                                break;
                            }
                            mem_service_stash_pending_desc(rt, node_idx, &rx);
                        }
                    }
                    if (candidates_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                        continue;
                    }
                    if (!terminal_slot->region.addr &&
                        mem_service_activate_remote_slot(rt, node_idx) != 0) {
                        continue;
                    }
                    memset(&compact, 0, sizeof(compact));
                    memset(&seen, 0, sizeof(seen));
                    candidates_record_found =
                        mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                      &compact,
                                                                      &seen) &&
                        mem_service_slot_find_record_by_obmm_object_backing(
                            terminal_slot,
                            MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            candidates_desc.payload_offset,
                            candidates_desc.payload_len,
                            candidates_desc.cookie,
                            &candidates_record);
                }
                if (candidates_record_found &&
                    candidates_record.kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.version == 1U &&
                    candidates_record.object_payload_kind ==
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.object_backing_len ==
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES &&
                    terminal_slot->region.addr &&
                    candidates_record.object_backing_offset +
                            candidates_record.object_backing_len <=
                        terminal_slot->region.len) {
                    candidate_owner_idx = node_idx;
                    break;
                }
                memset(&candidates_desc, 0, sizeof(candidates_desc));
            }
            if (candidate_owner_idx < 0) {
                usleep(10000);
                continue;
            }
            terminal_slot = &rt->slots[candidate_owner_idx];
            if (candidates_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.version != 1U ||
                candidates_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES ||
                !terminal_slot->region.addr ||
                candidates_record.object_backing_offset + candidates_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(candidate_words, 0, sizeof(candidate_words));
            memcpy(candidate_words,
                   (uint8_t *)terminal_slot->region.addr +
                       candidates_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                  31U * sizeof(uint64_t));
            candidates_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                       MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            candidate_count = candidate_words[1];
            if (candidate_words[0] == decode_step &&
                candidate_count > 0 &&
                candidate_count <= 4U &&
                candidate_count <= candidate_capacity &&
                candidate_words[31] == inner_checksum &&
                candidates_checksum == candidates_record.object_payload_checksum) {
                for (uint64_t i = 0; i < candidate_count; ++i) {
                    uint64_t base = 2U + i * 7U;

                    candidate_tokens_out[i] = candidate_words[base + 1U];
                    if (candidate_logit_bits_out) {
                        candidate_logit_bits_out[i] = candidate_words[base + 2U];
                    }
                    if (candidate_text_checksums_out) {
                        candidate_text_checksums_out[i] = candidate_words[base + 3U];
                    }
                    if (candidate_piece_bytes_out) {
                        candidate_piece_bytes_out[i] = candidate_words[base + 4U];
                    }
                    if (candidate_piece_word0_out) {
                        candidate_piece_word0_out[i] = candidate_words[base + 5U];
                    }
                    if (candidate_piece_word1_out) {
                        candidate_piece_word1_out[i] = candidate_words[base + 6U];
                    }
                }
                *candidate_count_out = candidate_count;
                if (candidate_checksum_out) {
                    *candidate_checksum_out = candidates_checksum;
                }
                printf("[mem_service] stage qwen3_engram_candidates_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " candidate_count=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       candidates_key,
                       candidate_owner_idx + 1,
                       candidates_record.version,
                       candidate_count,
                       candidates_record.object_backing_len,
                       candidates_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_candidates_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           candidates_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_selected_token(struct mem_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *selected_token_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char selected_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc) {
        return -1;
    }
    mem_service_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record selected_record;
        struct obmm_desc selected_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        uint16_t expected_epoch;
        bool selected_record_found = false;

        memset(&selected_record, 0, sizeof(selected_record));
        memset(&selected_desc, 0, sizeof(selected_desc));
        expected_epoch = (uint16_t)(decode_step + 1U);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                selected_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           selected_key,
                                           &selected_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        &selected_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES)) {
                            selected_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (selected_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                selected_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        selected_desc.payload_offset,
                        selected_desc.payload_len,
                        selected_desc.cookie,
                        &selected_record);
            }
            if (!selected_record_found ||
                selected_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES ||
                !terminal_slot->region.addr ||
                selected_record.object_backing_offset + selected_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       selected_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            if (payload_words[0] == decode_step &&
                checksum == selected_record.object_payload_checksum) {
                if (selected_token_out) {
                    *selected_token_out = payload_words[1];
                }
                printf("[mem_service] stage qwen3_engram_selected_token_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " bytes=%" PRIu64 " token=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       selected_key,
                       owner_idx + 1,
                       selected_record.version,
                       selected_record.object_backing_len,
                       payload_words[1],
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_selected_token_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           selected_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_history(struct mem_service *svc,
                                              uint64_t decode_step,
                                              uint64_t timeout_ms,
                                              uint64_t *history_tokens_out,
                                              uint64_t history_token_capacity,
                                              uint64_t *history_token_count_out,
                                              uint64_t *history_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char history_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());
    uint64_t expected_version = decode_step + 1U;

    if (!svc || !history_tokens_out || history_token_capacity == 0 ||
        !history_token_count_out) {
        return -1;
    }
    *history_token_count_out = 0;
    if (history_checksum_out) {
        *history_checksum_out = 0;
    }
    mem_service_qwen3_engram_history_key(history_key, sizeof(history_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record history_record;
        struct obmm_desc history_desc;
        uint64_t payload_words[1024 + 2];
        uint64_t checksum;
        uint64_t history_token_count;
        uint64_t expected_bytes;
        uint16_t expected_epoch;
        bool history_record_found = false;

        memset(&history_record, 0, sizeof(history_record));
        memset(&history_desc, 0, sizeof(history_desc));
        expected_epoch = (uint16_t)expected_version;
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                history_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           history_key,
                                           &history_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        3U * sizeof(uint64_t),
                        sizeof(payload_words),
                        &history_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                3U * sizeof(uint64_t),
                                sizeof(payload_words))) {
                            history_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (history_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                history_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        history_desc.payload_offset,
                        history_desc.payload_len,
                        history_desc.cookie,
                        &history_record);
            }
            if (!history_record_found ||
                history_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY ||
                history_record.version != expected_version ||
                history_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY ||
                history_record.object_backing_len < 3U * sizeof(uint64_t) ||
                history_record.object_backing_len > sizeof(payload_words) ||
                !terminal_slot->region.addr ||
                history_record.object_backing_offset + history_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(payload_words, 0, sizeof(payload_words));
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       history_record.object_backing_offset,
                   history_record.object_backing_len);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            history_record.object_backing_len);
            history_token_count = payload_words[1];
            expected_bytes = (history_token_count + 2U) * sizeof(uint64_t);
            if (payload_words[0] == expected_version &&
                history_token_count > 0 &&
                history_token_count <= history_token_capacity &&
                expected_bytes == history_record.object_backing_len &&
                checksum == history_record.object_payload_checksum) {
                memcpy(history_tokens_out,
                       &payload_words[2],
                       history_token_count * sizeof(uint64_t));
                *history_token_count_out = history_token_count;
                if (history_checksum_out) {
                    *history_checksum_out = checksum;
                }
                printf("[mem_service] stage qwen3_engram_history_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       history_key,
                       owner_idx + 1,
                       history_record.version,
                       history_token_count,
                       history_record.object_backing_len,
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_history_wait=timeout step=%" PRIu64
           " object_key=%s expected_version=%" PRIu64 "\n",
           decode_step,
           history_key,
           expected_version);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_state(struct mem_service *svc,
                                            uint64_t decode_step,
                                            uint64_t timeout_ms,
                                            uint64_t expected_history_token_count,
                                            uint64_t expected_selected_token,
                                            uint64_t expected_history_checksum,
                                            uint64_t no_repeat_ngram_size,
                                            uint64_t repetition_penalty_milli,
                                            uint64_t *state_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char state_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc || expected_history_token_count == 0) {
        return -1;
    }
    if (state_checksum_out) {
        *state_checksum_out = 0;
    }
    mem_service_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record state_record;
        struct obmm_desc state_desc;
        uint64_t state_words[16];
        uint64_t inner_checksum;
        uint64_t state_checksum;
        bool state_record_found = false;

        memset(&state_record, 0, sizeof(state_record));
        memset(&state_desc, 0, sizeof(state_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                state_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           state_key,
                                           &state_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint16_t expected_epoch = (uint16_t)(decode_step + 1U);

                if (expected_epoch == 0) {
                    expected_epoch = 1;
                }
                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        &state_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES)) {
                            state_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (state_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                state_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        state_desc.payload_offset,
                        state_desc.payload_len,
                        state_desc.cookie,
                        &state_record);
            }
            if (!state_record_found ||
                state_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE ||
                state_record.version != 1U ||
                state_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE ||
                state_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES ||
                !terminal_slot->region.addr ||
                state_record.object_backing_offset + state_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(state_words, 0, sizeof(state_words));
            memcpy(state_words,
                   (uint8_t *)terminal_slot->region.addr +
                       state_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  15U * sizeof(uint64_t));
            state_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            if (state_words[0] == decode_step &&
                state_words[1] == expected_history_token_count &&
                state_words[2] == expected_selected_token &&
                state_words[3] == expected_history_checksum &&
                state_words[4] == no_repeat_ngram_size &&
                state_words[5] == repetition_penalty_milli &&
                state_words[15] == inner_checksum &&
                state_checksum == state_record.object_payload_checksum) {
                if (state_checksum_out) {
                    *state_checksum_out = state_checksum;
                }
                printf("[mem_service] stage qwen3_engram_state_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " selected_token=%" PRIu64
                       " history_checksum=0x%016" PRIx64
                       " blocked=%" PRIu64 " fallback=%" PRIu64
                       " raw_token=%" PRIu64 " runner_up=%" PRIu64
                       " top_score_milli=%" PRId64
                       " runner_up_score_milli=%" PRId64
                       " history_window=%" PRIu64
                       " logits_checksum=0x%016" PRIx64
                       " text_checksum=0x%016" PRIx64
                       " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       state_key,
                       owner_idx + 1,
                       state_record.version,
                       state_words[1],
                       state_words[2],
                       state_words[3],
                       state_words[6],
                       state_words[7],
                       state_words[8],
                       state_words[9],
                       (int64_t)state_words[10],
                       (int64_t)state_words[11],
                       state_words[12],
                       state_words[13],
                       state_words[14],
                       state_record.object_backing_len,
                       state_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_state_wait=timeout step=%" PRIu64
           " object_key=%s expected_history_tokens=%" PRIu64
           " expected_selected_token=%" PRIu64
           " expected_history_checksum=0x%016" PRIx64 "\n",
           decode_step,
           state_key,
           expected_history_token_count,
           expected_selected_token,
           expected_history_checksum);
    return -1;
}

#include "mem_service_qwen3_decode_barrier.inc"
