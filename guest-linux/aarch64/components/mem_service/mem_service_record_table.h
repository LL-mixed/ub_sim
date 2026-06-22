#ifndef MEM_SERVICE_RECORD_TABLE_H
#define MEM_SERVICE_RECORD_TABLE_H

#include "mem_service.h"

#include <stdbool.h>

static struct mem_service_record *mem_service_alloc_record(struct mem_service *svc);
static struct mem_service_record *mem_service_find_record(struct mem_service *svc, const char *key);
static bool mem_service_record_has_member(const struct mem_service_record *rec,
                                          const char *block_hash);
static int mem_service_add_member(struct mem_service_record *rec, const char *block_hash);

#endif
