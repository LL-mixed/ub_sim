#include "mem_service_cluster_read.h"

#include "mem_service_cluster_utils.h"
#include "mem_service_compiler.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool mem_service_try_read_stable_payload(const struct mem_service_cluster_payload *payload,
                                          struct mem_service_cluster_payload *snapshot)
{
    if (!payload || !snapshot) {
        return false;
    }
    {
        struct mem_service_cluster_payload_header header;
        uint16_t i;

        __sync_synchronize();
        header.magic = payload->magic;
        header.version = payload->version;
        header.record_count = payload->record_count;
        header.publish_seq = payload->publish_seq;
        header.publish_done_seq = payload->publish_done_seq;
        if (header.publish_seq == 0 ||
            header.publish_seq != header.publish_done_seq ||
            header.magic != MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC ||
            header.version != MEM_SERVICE_CLUSTER_PAYLOAD_VERSION ||
            header.record_count == 0 ||
            header.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
            return false;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->magic = header.magic;
        snapshot->version = header.version;
        snapshot->record_count = header.record_count;
        snapshot->publish_seq = header.publish_seq;
        snapshot->publish_done_seq = header.publish_done_seq;
        for (i = 0; i < header.record_count; ++i) {
            memcpy(&snapshot->records[i], &payload->records[i], sizeof(snapshot->records[i]));
        }
        __sync_synchronize();
        if (snapshot->publish_seq == snapshot->publish_done_seq &&
            snapshot->publish_seq == header.publish_seq &&
            snapshot->publish_done_seq == header.publish_done_seq &&
            snapshot->magic == MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC &&
            snapshot->version == MEM_SERVICE_CLUSTER_PAYLOAD_VERSION &&
            snapshot->record_count == header.record_count) {
            return true;
        }
    }
    return false;
}

static bool mem_service_read_stable_payload(const struct mem_service_cluster_payload *payload,
                                      struct mem_service_cluster_payload *snapshot)
{
    int attempts = 8;

    while (attempts-- > 0) {
        if (mem_service_try_read_stable_payload(payload, snapshot)) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

static void mem_service_copy_from_mapped_volatile(void *dst,
                                            const volatile uint8_t *src,
                                            size_t len)
{
    size_t i = 0;
    uint8_t *out = (uint8_t *)dst;

    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t word = *(const volatile uint64_t *)(src + i);
        memcpy(out + i, &word, sizeof(word));
    }
    for (; i < len; ++i) {
        out[i] = src[i];
    }
}

bool mem_service_try_read_stable_payload_region(const struct mem_service_cluster_slot *slot,
                                                struct mem_service_cluster_payload *snapshot,
                                                struct mem_service_cluster_payload_header *seen_out)
{
    struct mem_service_cluster_payload_header header;
    struct mem_service_cluster_payload_header confirm;
    uint16_t i;
    const volatile uint8_t *mapped_bytes;

    mapped_bytes = slot ? (const volatile uint8_t *)slot->region.addr : NULL;

    if (!slot || !snapshot) {
        return false;
    }
    if (slot->is_local) {
        bool ok = mem_service_try_read_stable_payload((const struct mem_service_cluster_payload *)slot->region.addr,
                                                snapshot);
        if (ok && seen_out) {
            seen_out->magic = snapshot->magic;
            seen_out->version = snapshot->version;
            seen_out->record_count = snapshot->record_count;
            seen_out->publish_seq = snapshot->publish_seq;
            seen_out->publish_done_seq = snapshot->publish_done_seq;
        }
        return ok;
    }
    if (slot->region.fd < 0) {
        return false;
    }
    printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=read_header_begin mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
           slot->owner_idx + 1,
           slot->reader_idx + 1,
           slot->mem_id,
           slot->map_osync ? 1 : 0,
           slot->region.addr);
    fflush(stdout);
    mem_service_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=read_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           header.publish_seq,
           header.publish_done_seq,
           header.record_count);
    fflush(stdout);
    if (seen_out) {
        *seen_out = header;
    }
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC ||
        header.version != MEM_SERVICE_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = header.magic;
    snapshot->version = header.version;
    snapshot->record_count = header.record_count;
    snapshot->publish_seq = header.publish_seq;
    snapshot->publish_done_seq = header.publish_done_seq;
    for (i = 0; i < header.record_count; ++i) {
        size_t record_off = offsetof(struct mem_service_cluster_payload, records) +
                            ((size_t)i * sizeof(snapshot->records[0]));
        printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_begin record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               slot->reader_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
        mem_service_copy_from_mapped_volatile(&snapshot->records[i],
                                        mapped_bytes + record_off,
                                        sizeof(snapshot->records[i]));
        printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_done record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               slot->reader_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
    }
    __sync_synchronize();
    printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_begin\n",
           slot->owner_idx + 1,
           slot->reader_idx + 1);
    fflush(stdout);
    mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           slot->reader_idx + 1,
           confirm.publish_seq,
           confirm.publish_done_seq,
           confirm.record_count);
    fflush(stdout);
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return true;
}

bool mem_service_try_read_stable_compact_summary_region(
    const struct mem_service_cluster_slot *slot,
    struct mem_service_cluster_payload_compact_summary *summary,
    struct mem_service_cluster_payload_header *seen_out)
{
    struct mem_service_cluster_payload_header header;
    struct mem_service_cluster_payload_header confirm;
    const volatile uint8_t *mapped_bytes;
    size_t summary_off = offsetof(struct mem_service_cluster_payload, record_pad);

    if (!slot || !summary || !slot->region.addr) {
        return false;
    }
    if (slot->is_local) {
        const struct mem_service_cluster_payload *payload =
            (const struct mem_service_cluster_payload *)slot->region.addr;

        if (!mem_service_try_read_stable_payload(payload,
                                           &(struct mem_service_cluster_payload){ 0 })) {
            return false;
        }
        memcpy(summary, payload->record_pad, sizeof(*summary));
        if (seen_out) {
            seen_out->magic = payload->magic;
            seen_out->version = payload->version;
            seen_out->record_count = payload->record_count;
            seen_out->publish_seq = payload->publish_seq;
            seen_out->publish_done_seq = payload->publish_done_seq;
        }
        return true;
    }
    mapped_bytes = (const volatile uint8_t *)slot->region.addr;
    if (slot->region.fd < 0) {
        return false;
    }
    mem_service_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    if (seen_out) {
        *seen_out = header;
    }
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC ||
        header.version != MEM_SERVICE_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
        return false;
    }
    memset(summary, 0, sizeof(*summary));
    mem_service_copy_from_mapped_volatile(summary, mapped_bytes + summary_off, sizeof(*summary));
    __sync_synchronize();
    mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count ||
        summary->record_count != header.record_count) {
        return false;
    }
    return true;
}

bool mem_service_wait_compact_summary_region_at_least(
    const struct mem_service_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct mem_service_cluster_payload_compact_summary *summary,
    struct mem_service_cluster_payload_header *seen_out)
{
    long deadline;
    unsigned int relax_attempt = 0;
    struct mem_service_cluster_payload_compact_summary local_summary;
    struct mem_service_cluster_payload_header local_seen;

    if (!slot || !summary) {
        return false;
    }
    deadline = obmm_now_ms() + timeout_ms;
    while (obmm_now_ms() < deadline) {
        memset(&local_summary, 0, sizeof(local_summary));
        memset(&local_seen, 0, sizeof(local_seen));
        if (mem_service_try_read_stable_compact_summary_region(slot, &local_summary, &local_seen)) {
            if (seen_out) {
                *seen_out = local_seen;
            }
            if (local_seen.publish_done_seq >= min_publish_done_seq) {
                *summary = local_summary;
                return true;
            }
        } else if (seen_out) {
            *seen_out = local_seen;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool mem_service_read_stable_payload_region(const struct mem_service_cluster_slot *slot,
                                             struct mem_service_cluster_payload *snapshot,
                                             struct mem_service_cluster_payload_header *seen_out)
{
    int attempts = 8;
    unsigned int relax_attempt = 0;

    while (attempts-- > 0) {
        if (mem_service_try_read_stable_payload_region(slot, snapshot, seen_out)) {
            return true;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool MEM_SERVICE_MAYBE_UNUSED mem_service_wait_stable_payload_region_at_least(
    const struct mem_service_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct mem_service_cluster_payload *snapshot,
    struct mem_service_cluster_payload_header *seen_out)
{
    long deadline;
    unsigned int relax_attempt = 0;
    struct mem_service_cluster_payload local_snapshot;
    struct mem_service_cluster_payload_header local_seen;

    if (!slot || !snapshot) {
        return false;
    }
    deadline = obmm_now_ms() + timeout_ms;
    while (obmm_now_ms() < deadline) {
        memset(&local_snapshot, 0, sizeof(local_snapshot));
        memset(&local_seen, 0, sizeof(local_seen));
        if (mem_service_try_read_stable_payload_region(slot, &local_snapshot, &local_seen)) {
            if (seen_out) {
                *seen_out = local_seen;
            }
            if (local_snapshot.publish_done_seq >= min_publish_done_seq) {
                *snapshot = local_snapshot;
                return true;
            }
        } else if (seen_out) {
            *seen_out = local_seen;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool MEM_SERVICE_MAYBE_UNUSED mem_service_payload_find_record(
    const struct mem_service_cluster_payload *payload,
    const char *key,
    struct mem_service_record *resolved_out)
{
    struct mem_service_cluster_payload snapshot;
    uint16_t i;

    if (!payload || !key || !resolved_out) {
        return false;
    }
    if (!mem_service_read_stable_payload(payload, &snapshot)) {
        return false;
    }
    for (i = 0; i < snapshot.record_count; ++i) {
        if (!snapshot.records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
            *resolved_out = snapshot.records[i];
            return true;
        }
    }
    return false;
}

static bool MEM_SERVICE_MAYBE_UNUSED mem_service_payload_snapshot_find_record(
    const struct mem_service_cluster_payload *snapshot,
    const char *key,
    struct mem_service_record *resolved_out)
{
    uint16_t i;

    if (!snapshot || !key || !resolved_out) {
        return false;
    }
    for (i = 0; i < snapshot->record_count; ++i) {
        if (!snapshot->records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot->records[i].key, key, sizeof(snapshot->records[i].key)) == 0) {
            *resolved_out = snapshot->records[i];
            return true;
        }
    }
    return false;
}

bool mem_service_slot_find_record(const struct mem_service_cluster_slot *slot,
                                  const char *key,
                                  struct mem_service_record *resolved_out)
{
    struct mem_service_cluster_payload_header header;
    struct mem_service_cluster_payload_header confirm;
    const volatile uint8_t *mapped_bytes;
    uint16_t i;

    if (!slot || !key || !resolved_out) {
        return false;
    }
    if (slot->is_local) {
        struct mem_service_cluster_payload snapshot;

        if (!mem_service_read_stable_payload_region(slot, &snapshot, NULL)) {
            return false;
        }
        for (i = 0; i < snapshot.record_count; ++i) {
            if (!snapshot.records[i].in_use) {
                continue;
            }
            if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
                *resolved_out = snapshot.records[i];
                return true;
            }
        }
        return false;
    }
    if (!slot->region.addr || slot->region.fd < 0) {
        return false;
    }
    mapped_bytes = (const volatile uint8_t *)slot->region.addr;
    mem_service_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC ||
        header.version != MEM_SERVICE_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
        return false;
    }
    for (i = 0; i < header.record_count; ++i) {
        bool in_use = false;
        enum mem_service_record_kind kind = 0;
        char record_key[sizeof(resolved_out->key)];
        size_t record_off = offsetof(struct mem_service_cluster_payload, records) +
                            ((size_t)i * sizeof(struct mem_service_record));

        memset(record_key, 0, sizeof(record_key));
        mem_service_copy_from_mapped_volatile(&in_use,
                                        mapped_bytes + record_off +
                                            offsetof(struct mem_service_record, in_use),
                                        sizeof(in_use));
        if (!in_use) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(&kind,
                                        mapped_bytes + record_off +
                                            offsetof(struct mem_service_record, kind),
                                        sizeof(kind));
        if (kind < MEM_SERVICE_RECORD_PREFIX_GROUP ||
            kind > MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE) {
            return false;
        }
        mem_service_copy_from_mapped_volatile(record_key,
                                        mapped_bytes + record_off +
                                            offsetof(struct mem_service_record, key),
                                        sizeof(record_key));
        if (strncmp(record_key, key, sizeof(record_key)) == 0) {
            mem_service_copy_from_mapped_volatile(resolved_out,
                                            mapped_bytes + record_off,
                                            sizeof(*resolved_out));
            __sync_synchronize();
            mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
            if (confirm.publish_seq != header.publish_seq ||
                confirm.publish_done_seq != header.publish_done_seq ||
                confirm.magic != header.magic ||
                confirm.version != header.version ||
                confirm.record_count != header.record_count) {
                return false;
            }
            return true;
        }
    }
    __sync_synchronize();
    mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return false;
}

static bool mem_service_record_matches_obmm_object_backing(
    const struct mem_service_record *record,
    enum mem_service_record_kind record_kind,
    uint32_t payload_kind,
    uint64_t payload_offset,
    uint64_t payload_len,
    uint32_t checksum_cookie)
{
    uint32_t record_cookie;

    if (!record || !record->in_use || record->kind != record_kind ||
        record->object_payload_kind != payload_kind ||
        record->object_backing_offset != payload_offset ||
        record->object_backing_len != payload_len) {
        return false;
    }
    record_cookie = (uint32_t)(record->object_payload_checksum ^
                               (record->object_payload_checksum >> 32));
    return record_cookie == checksum_cookie;
}

bool mem_service_slot_find_record_by_obmm_object_backing(
    const struct mem_service_cluster_slot *slot,
    enum mem_service_record_kind record_kind,
    uint32_t payload_kind,
    uint64_t payload_offset,
    uint64_t payload_len,
    uint32_t checksum_cookie,
    struct mem_service_record *resolved_out)
{
    struct mem_service_cluster_payload_header header;
    struct mem_service_cluster_payload_header confirm;
    const volatile uint8_t *mapped_bytes;
    uint16_t i;

    if (!slot || !resolved_out) {
        return false;
    }
    if (slot->is_local) {
        struct mem_service_cluster_payload snapshot;

        if (!mem_service_read_stable_payload_region(slot, &snapshot, NULL)) {
            return false;
        }
        for (i = 0; i < snapshot.record_count; ++i) {
            if (mem_service_record_matches_obmm_object_backing(
                    &snapshot.records[i],
                    record_kind,
                    payload_kind,
                    payload_offset,
                    payload_len,
                    checksum_cookie)) {
                *resolved_out = snapshot.records[i];
                return true;
            }
        }
        return false;
    }
    if (!slot->region.addr || slot->region.fd < 0) {
        return false;
    }
    mapped_bytes = (const volatile uint8_t *)slot->region.addr;
    mem_service_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC ||
        header.version != MEM_SERVICE_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
        return false;
    }
    for (i = 0; i < header.record_count; ++i) {
        bool in_use = false;
        enum mem_service_record_kind kind = 0;
        uint32_t object_payload_kind = 0;
        uint64_t object_backing_offset = 0;
        uint64_t object_backing_len = 0;
        uint64_t object_payload_checksum = 0;
        uint32_t object_cookie;
        size_t record_off = offsetof(struct mem_service_cluster_payload, records) +
                            ((size_t)i * sizeof(struct mem_service_record));

        mem_service_copy_from_mapped_volatile(&in_use,
                                        mapped_bytes + record_off +
                                            offsetof(struct mem_service_record, in_use),
                                        sizeof(in_use));
        if (!in_use) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(&kind,
                                        mapped_bytes + record_off +
                                            offsetof(struct mem_service_record, kind),
                                        sizeof(kind));
        if (kind < MEM_SERVICE_RECORD_PREFIX_GROUP ||
            kind > MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE) {
            return false;
        }
        if (kind != record_kind) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(
            &object_payload_kind,
            mapped_bytes + record_off +
                offsetof(struct mem_service_record, object_payload_kind),
            sizeof(object_payload_kind));
        if (object_payload_kind != payload_kind) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(
            &object_backing_offset,
            mapped_bytes + record_off +
                offsetof(struct mem_service_record, object_backing_offset),
            sizeof(object_backing_offset));
        mem_service_copy_from_mapped_volatile(
            &object_backing_len,
            mapped_bytes + record_off +
                offsetof(struct mem_service_record, object_backing_len),
            sizeof(object_backing_len));
        if (object_backing_offset != payload_offset ||
            object_backing_len != payload_len) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(
            &object_payload_checksum,
            mapped_bytes + record_off +
                offsetof(struct mem_service_record, object_payload_checksum),
            sizeof(object_payload_checksum));
        object_cookie = (uint32_t)(object_payload_checksum ^
                                   (object_payload_checksum >> 32));
        if (object_cookie != checksum_cookie) {
            continue;
        }
        mem_service_copy_from_mapped_volatile(resolved_out,
                                        mapped_bytes + record_off,
                                        sizeof(*resolved_out));
        __sync_synchronize();
        mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
        if (confirm.publish_seq != header.publish_seq ||
            confirm.publish_done_seq != header.publish_done_seq ||
            confirm.magic != header.magic ||
            confirm.version != header.version ||
            confirm.record_count != header.record_count) {
            return false;
        }
        return true;
    }
    __sync_synchronize();
    mem_service_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return false;
}
