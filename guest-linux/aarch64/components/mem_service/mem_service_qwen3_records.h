#ifndef MEM_SERVICE_QWEN3_RECORDS_H
#define MEM_SERVICE_QWEN3_RECORDS_H

#include "mem_service.h"
#include "mem_service_qwen3_record_policy.h"

#include <stdbool.h>
#include <stdint.h>

static bool mem_service_qwen3_record_kind_recyclable(enum mem_service_record_kind kind);
static bool mem_service_qwen3_key_decode_step(const char *key, uint64_t *step_out);
static struct mem_service_record *mem_service_recycle_qwen3_runtime_record(
    struct mem_service *svc,
    const char *incoming_key);

#endif
