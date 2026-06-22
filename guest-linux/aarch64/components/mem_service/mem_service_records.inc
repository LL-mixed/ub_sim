#include "mem_service_record_table.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static struct mem_service_record *mem_service_alloc_record(struct mem_service *svc)
{
    size_t i;

    if (!svc) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            svc->records[i].in_use = true;
            svc->record_count += 1;
            return &svc->records[i];
        }
    }
    return NULL;
}

static struct mem_service_record *mem_service_find_record(struct mem_service *svc, const char *key)
{
    size_t i;

    if (!svc || !key) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        if (strncmp(svc->records[i].key, key, sizeof(svc->records[i].key)) == 0) {
            return &svc->records[i];
        }
    }
    return NULL;
}

static bool mem_service_record_has_member(const struct mem_service_record *rec, const char *block_hash)
{
    uint32_t i;
    if (!rec || !block_hash) {
        return false;
    }
    for (i = 0; i < rec->member_count && i < MEM_SERVICE_MAX_GROUP_MEMBERS; ++i) {
        if (strncmp(rec->member_block_hashes[i], block_hash, sizeof(rec->member_block_hashes[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool mem_service_record_has_member_block(const struct mem_service_record *rec, const char *block_hash)
{
    return mem_service_record_has_member(rec, block_hash);
}

static int mem_service_add_member(struct mem_service_record *rec, const char *block_hash)
{
    if (!rec || !block_hash) {
        return -1;
    }
    if (mem_service_record_has_member(rec, block_hash)) {
        return 0;
    }
    if (rec->member_count >= MEM_SERVICE_MAX_GROUP_MEMBERS) {
        return -1;
    }
    snprintf(rec->member_block_hashes[rec->member_count],
             sizeof(rec->member_block_hashes[rec->member_count]),
             "%s",
             block_hash);
    rec->member_count += 1;
    return 0;
}
