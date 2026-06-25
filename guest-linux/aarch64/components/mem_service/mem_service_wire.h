#ifndef MEM_SERVICE_WIRE_H
#define MEM_SERVICE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_SERVICE_WIRE_MAGIC 0x4d535643U
#define MEM_SERVICE_WIRE_VERSION 1U
#define MEM_SERVICE_WIRE_HEADER_LEN 48U
#define MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN 4096U

enum mem_service_wire_operation {
    MEM_SERVICE_WIRE_OP_HEALTH = 1,
    MEM_SERVICE_WIRE_OP_READY = 2,
    MEM_SERVICE_WIRE_OP_STATUS = 3,
    MEM_SERVICE_WIRE_OP_LIST_RECORDS = 4,
    MEM_SERVICE_WIRE_OP_METRICS = 5,
    MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT = 6,
    MEM_SERVICE_WIRE_OP_EXPORT_SNAPSHOT_PAGE = 7,
    MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT = 8,
    MEM_SERVICE_WIRE_OP_RESTORE_SNAPSHOT_PAGE = 9,
    MEM_SERVICE_WIRE_OP_PUT_OBJECT = 16,
    MEM_SERVICE_WIRE_OP_GET_OBJECT = 17,
    MEM_SERVICE_WIRE_OP_INSPECT_OBJECT = 18,
    MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY = 32,
    MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY = 33,
    MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT = 48,
    MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT = 49,
    MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF = 64,
    MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF = 65,
    MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT = 80,
    MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT = 81,
    MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT = 96,
    MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT = 97,
};

enum mem_service_wire_status {
    MEM_SERVICE_WIRE_STATUS_OK = 0,
    MEM_SERVICE_WIRE_STATUS_NOT_FOUND = 1,
    MEM_SERVICE_WIRE_STATUS_STALE_REF = 2,
    MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH = 3,
    MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT = 4,
    MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING = 5,
    MEM_SERVICE_WIRE_STATUS_INVALID_SESSION = 6,
    MEM_SERVICE_WIRE_STATUS_TIMEOUT = 7,
    MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED = 8,
    MEM_SERVICE_WIRE_STATUS_UNSUPPORTED = 9,
    MEM_SERVICE_WIRE_STATUS_INTERNAL = 10,
};

struct mem_service_wire_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint64_t request_id;
    uint32_t operation;
    uint32_t flags;
    uint32_t payload_len;
    uint32_t payload_checksum;
    uint32_t status;
    uint32_t error_code;
    uint64_t server_time_ms;
};

typedef char mem_service_wire_header_size_must_be_48[
    (sizeof(struct mem_service_wire_header) == MEM_SERVICE_WIRE_HEADER_LEN) ? 1 : -1];

static inline uint32_t mem_service_wire_checksum(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261U;
    size_t i;

    if (len == 0) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static inline void mem_service_wire_init_header(struct mem_service_wire_header *header,
                                                uint64_t request_id,
                                                enum mem_service_wire_operation operation,
                                                uint32_t payload_len,
                                                uint32_t payload_checksum)
{
    header->magic = MEM_SERVICE_WIRE_MAGIC;
    header->version = MEM_SERVICE_WIRE_VERSION;
    header->header_len = MEM_SERVICE_WIRE_HEADER_LEN;
    header->request_id = request_id;
    header->operation = (uint32_t)operation;
    header->flags = 0;
    header->payload_len = payload_len;
    header->payload_checksum = payload_checksum;
    header->status = MEM_SERVICE_WIRE_STATUS_OK;
    header->error_code = MEM_SERVICE_WIRE_STATUS_OK;
    header->server_time_ms = 0;
}

static inline bool mem_service_wire_header_is_compatible(
    const struct mem_service_wire_header *header)
{
    return header->magic == MEM_SERVICE_WIRE_MAGIC &&
           header->version == MEM_SERVICE_WIRE_VERSION &&
           header->header_len == MEM_SERVICE_WIRE_HEADER_LEN;
}

#endif
