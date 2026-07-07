#ifndef MEM_SERVICE_OBJECT_REFS_H
#define MEM_SERVICE_OBJECT_REFS_H

#include "mem_service.h"

uint64_t mem_service_checksum_bytes(const uint8_t *bytes, uint64_t len);
int mem_service_record_to_lingqu_object_ref(const struct mem_service_record *record,
                                            struct lingqu_object_ref_wire *ref_out);
int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                          struct lingqu_object_ref_wire *ref_out);

#endif
