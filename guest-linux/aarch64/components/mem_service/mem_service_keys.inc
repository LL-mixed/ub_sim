static int mem_service_build_two_part_key(const char *prefix,
                                    const char *first,
                                    const char *middle,
                                    const char *second,
                                    char *out,
                                    size_t out_len)
{
    size_t prefix_len;
    size_t first_len;
    size_t middle_len;
    size_t second_len;
    size_t total_len;
    char *cursor;

    if (!prefix || !first || !middle || !second || !out || out_len == 0) {
        return -1;
    }
    prefix_len = strlen(prefix);
    first_len = strlen(first);
    middle_len = strlen(middle);
    second_len = strlen(second);
    if (prefix_len > SIZE_MAX - first_len ||
        prefix_len + first_len > SIZE_MAX - middle_len ||
        prefix_len + first_len + middle_len > SIZE_MAX - second_len) {
        out[0] = '\0';
        return -1;
    }
    total_len = prefix_len + first_len + middle_len + second_len;
    if (total_len + 1U > out_len) {
        out[0] = '\0';
        return -1;
    }
    cursor = out;
    memcpy(cursor, prefix, prefix_len);
    cursor += prefix_len;
    memcpy(cursor, first, first_len);
    cursor += first_len;
    memcpy(cursor, middle, middle_len);
    cursor += middle_len;
    memcpy(cursor, second, second_len);
    cursor += second_len;
    *cursor = '\0';
    return 0;
}

static int mem_service_build_prefix_key_from_parts_checked(const char *request_id,
                                                     const char *prefix_group,
                                                     char *out,
                                                     size_t out_len)
{
    return mem_service_build_two_part_key("request/",
                                    request_id,
                                    "/prefix/",
                                    prefix_group,
                                    out,
                                    out_len);
}

static int mem_service_build_group_key_from_parts_checked(const char *request_id,
                                                    const char *group_id,
                                                    char *out,
                                                    size_t out_len)
{
    return mem_service_build_two_part_key("request/",
                                    request_id,
                                    "/prefix-group/",
                                    group_id,
                                    out,
                                    out_len);
}

static int mem_service_build_block_key_from_hash_checked(const char *block_hash,
                                                   char *out,
                                                   size_t out_len)
{
    return mem_service_build_two_part_key("block/",
                                    block_hash,
                                    "",
                                    "",
                                    out,
                                    out_len);
}

static int mem_service_build_group_key(const struct mem_service_block_ctx *ctx,
                                 char *out,
                                 size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return mem_service_build_group_key_from_parts_checked(ctx->request_id,
                                                    ctx->group_id,
                                                    out,
                                                    out_len);
}

static int mem_service_build_prefix_key(const struct mem_service_block_ctx *ctx,
                                  char *out,
                                  size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return mem_service_build_prefix_key_from_parts_checked(ctx->request_id,
                                                    ctx->prefix_group,
                                                    out,
                                                    out_len);
}

static int mem_service_build_block_key(const struct mem_service_block_ctx *ctx,
                                 char *out,
                                 size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return mem_service_build_block_key_from_hash_checked(ctx->block_hash, out, out_len);
}

void mem_service_build_prefix_key_from_parts(const char *request_id,
                                       const char *prefix_group,
                                       char *out,
                                       size_t out_len)
{
    (void)mem_service_build_prefix_key_from_parts_checked(request_id,
                                                   prefix_group,
                                                   out,
                                                   out_len);
}

void mem_service_build_group_key_from_parts(const char *request_id,
                                      const char *group_id,
                                      char *out,
                                      size_t out_len)
{
    (void)mem_service_build_group_key_from_parts_checked(request_id,
                                                  group_id,
                                                  out,
                                                  out_len);
}

void mem_service_build_block_key_from_hash(const char *block_hash, char *out, size_t out_len)
{
    (void)mem_service_build_block_key_from_hash_checked(block_hash, out, out_len);
}
