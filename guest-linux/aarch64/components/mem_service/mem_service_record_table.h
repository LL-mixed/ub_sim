#ifndef MEM_SERVICE_RECORD_TABLE_H
#define MEM_SERVICE_RECORD_TABLE_H

#include "mem_service.h"

#include <stdbool.h>

struct mem_service_record *mem_service_alloc_record(struct mem_service *svc);
struct mem_service_record *mem_service_find_record(struct mem_service *svc, const char *key);
bool mem_service_record_has_member(const struct mem_service_record *rec,
                                   const char *block_hash);
int mem_service_add_member(struct mem_service_record *rec, const char *block_hash);

#endif
