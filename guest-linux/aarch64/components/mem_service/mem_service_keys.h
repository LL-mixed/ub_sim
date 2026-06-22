#ifndef MEM_SERVICE_KEYS_H
#define MEM_SERVICE_KEYS_H

#include "mem_service.h"

int mem_service_build_two_part_key(const char *prefix,
                                   const char *first,
                                   const char *middle,
                                   const char *second,
                                   char *out,
                                   size_t out_len);
int mem_service_build_prefix_key_from_parts_checked(const char *request_id,
                                                    const char *prefix_group,
                                                    char *out,
                                                    size_t out_len);
int mem_service_build_group_key_from_parts_checked(const char *request_id,
                                                   const char *group_id,
                                                   char *out,
                                                   size_t out_len);
int mem_service_build_block_key_from_hash_checked(const char *block_hash,
                                                  char *out,
                                                  size_t out_len);
int mem_service_build_group_key(const struct mem_service_block_ctx *ctx,
                                char *out,
                                size_t out_len);
int mem_service_build_prefix_key(const struct mem_service_block_ctx *ctx,
                                 char *out,
                                 size_t out_len);
int mem_service_build_block_key(const struct mem_service_block_ctx *ctx,
                                char *out,
                                size_t out_len);

#endif
