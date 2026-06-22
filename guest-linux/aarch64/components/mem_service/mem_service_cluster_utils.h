#ifndef MEM_SERVICE_CLUSTER_UTILS_H
#define MEM_SERVICE_CLUSTER_UTILS_H

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>

#include "mem_service_guest_runtime.h"

long mem_service_wallclock_ms(void);
void mem_service_cpu_relax_wait(unsigned int *attempt);
bool mem_service_parse_ip_list(const char *csv,
                               char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                               int *count_out);
bool mem_service_resolve_cluster_nodes(char local_ip[INET_ADDRSTRLEN],
                                       char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                       int *node_count,
                                       int *local_idx);
bool mem_service_parse_hex_file_u64(const char *path, uint64_t *value);
int mem_service_update_region_range_at(const struct mem_service_cluster_slot *slot,
                                       uint64_t offset,
                                       uint64_t length,
                                       bool for_write);
int mem_service_update_region_range(const struct mem_service_cluster_slot *slot, bool for_write);
int mem_service_sync_remote_range(const struct mem_service_cluster_slot *slot,
                                  uint64_t offset,
                                  uint64_t length);

#endif
