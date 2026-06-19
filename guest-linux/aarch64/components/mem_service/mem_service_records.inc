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

static bool mem_service_qwen3_record_kind_recyclable(enum mem_service_record_kind kind)
{
    switch (kind) {
    case MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT:
    case MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT:
    case MEM_SERVICE_RECORD_KVCACHE_OBJECT:
    case MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT:
    case MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES:
    case MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED:
    case MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE:
        return true;
    default:
        return false;
    }
}

static bool mem_service_key_decode_step(const char *key, uint64_t *step_out)
{
    const char *needle;
    char *end = NULL;
    unsigned long long parsed;

    if (!key || !step_out) {
        return false;
    }
    needle = strstr(key, "decode-step");
    if (needle) {
        needle += strlen("decode-step");
    } else {
        needle = strstr(key, "/step/");
        if (!needle) {
            return false;
        }
        needle += strlen("/step/");
    }
    if (*needle < '0' || *needle > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoull(needle, &end, 10);
    if (errno != 0 || end == needle) {
        return false;
    }
    *step_out = (uint64_t)parsed;
    return true;
}

static struct mem_service_record *mem_service_recycle_qwen3_runtime_record(
    struct mem_service *svc,
    const char *incoming_key)
{
    struct mem_service_record *candidate = NULL;
    uint64_t incoming_step;
    uint64_t candidate_step = UINT64_MAX;
    size_t i;

    if (!svc || !mem_service_key_decode_step(incoming_key, &incoming_step) ||
        incoming_step <= MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        struct mem_service_record *rec = &svc->records[i];
        uint64_t rec_step;

        if (!rec->in_use ||
            !mem_service_qwen3_record_kind_recyclable(rec->kind) ||
            !mem_service_key_decode_step(rec->key, &rec_step) ||
            rec_step + MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS >= incoming_step) {
            continue;
        }
        if (!candidate || rec_step < candidate_step) {
            candidate = rec;
            candidate_step = rec_step;
        }
    }
    if (candidate) {
        printf("[mem_service] stage db_service_record_recycle key=%s old_step=%" PRIu64
               " incoming_step=%" PRIu64 " retain_steps=%" PRIu64
               " record_count=%zu status=ok\n",
               candidate->key,
               candidate_step,
               incoming_step,
               (uint64_t)MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS,
               svc->record_count);
        memset(candidate, 0, sizeof(*candidate));
    }
    return candidate;
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
