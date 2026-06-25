#ifndef MEM_SERVICE_WIRE_PAYLOAD_H
#define MEM_SERVICE_WIRE_PAYLOAD_H

#include <errno.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mem_service_wire_payload_field_type {
    MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING = 1,
    MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32 = 2,
    MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64 = 3,
};

struct mem_service_wire_payload_view {
    const char *data;
    size_t len;
};

struct mem_service_wire_payload_field {
    const char *name;
    enum mem_service_wire_payload_field_type type;
    bool required;
};

static inline struct mem_service_wire_payload_view
mem_service_wire_payload_view_from_cstr(const char *payload)
{
    struct mem_service_wire_payload_view view;

    view.data = payload != NULL ? payload : "";
    view.len = strlen(view.data);
    return view;
}

static inline bool mem_service_wire_payload_get_string(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    char *out,
    size_t out_len)
{
    size_t name_len;
    const char *cursor;

    if (view == NULL || name == NULL || out == NULL || out_len == 0) {
        return false;
    }
    name_len = strlen(name);
    cursor = view->data;
    out[0] = '\0';
    while (cursor != NULL && *cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_len > name_len &&
            strncmp(cursor, name, name_len) == 0 &&
            cursor[name_len] == '=') {
            size_t value_len = line_len - name_len - 1;

            if (value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, cursor + name_len + 1, value_len);
            out[value_len] = '\0';
            return out[0] != '\0';
        }
        cursor = line_end ? line_end + 1 : NULL;
    }
    return false;
}

static inline bool mem_service_wire_payload_get_u64_checked(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    uint64_t *out)
{
    char value[48];
    char *end = NULL;
    uint64_t parsed;

    if (out == NULL ||
        !mem_service_wire_payload_get_string(view, name, value, sizeof(value))) {
        return false;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

static inline uint64_t mem_service_wire_payload_get_u64(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    uint64_t default_value)
{
    uint64_t parsed;

    return mem_service_wire_payload_get_u64_checked(view, name, &parsed)
               ? parsed
               : default_value;
}

static inline uint32_t mem_service_wire_payload_get_u32(
    const struct mem_service_wire_payload_view *view,
    const char *name,
    uint32_t default_value)
{
    uint64_t parsed;

    if (!mem_service_wire_payload_get_u64_checked(view, name, &parsed) ||
        parsed > UINT32_MAX) {
        return default_value;
    }
    return (uint32_t)parsed;
}

static inline int mem_service_wire_payload_append_field(char *payload,
                                                        size_t payload_len,
                                                        const char *name,
                                                        const char *value)
{
    size_t used;
    int written;

    if (payload == NULL || payload_len == 0 || name == NULL) {
        return -1;
    }
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    used = strlen(payload);
    if (used >= payload_len) {
        return -1;
    }
    written = snprintf(payload + used, payload_len - used, "%s=%s\n", name, value);
    if (written < 0 || (size_t)written >= payload_len - used) {
        return -1;
    }
    return 0;
}

static inline int mem_service_wire_payload_append_u64(char *payload,
                                                      size_t payload_len,
                                                      const char *name,
                                                      uint64_t value)
{
    char text[32];

    snprintf(text, sizeof(text), "%" PRIu64, value);
    return mem_service_wire_payload_append_field(payload, payload_len, name, text);
}

static inline bool mem_service_wire_payload_validate_schema(
    const struct mem_service_wire_payload_view *view,
    const struct mem_service_wire_payload_field *schema,
    size_t schema_len,
    size_t *failed_index_out)
{
    size_t i;

    for (i = 0; i < schema_len; ++i) {
        const struct mem_service_wire_payload_field *field = &schema[i];
        char value[128];
        uint64_t parsed_u64;
        bool present;

        present = mem_service_wire_payload_get_string(view,
                                                      field->name,
                                                      value,
                                                      sizeof(value));
        if (!present) {
            if (field->required) {
                if (failed_index_out != NULL) {
                    *failed_index_out = i;
                }
                return false;
            }
            continue;
        }
        if (field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32) {
            if (!mem_service_wire_payload_get_u64_checked(view,
                                                          field->name,
                                                          &parsed_u64) ||
                parsed_u64 > UINT32_MAX) {
                if (failed_index_out != NULL) {
                    *failed_index_out = i;
                }
                return false;
            }
        } else if (field->type == MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64) {
            if (!mem_service_wire_payload_get_u64_checked(view,
                                                          field->name,
                                                          &parsed_u64)) {
                if (failed_index_out != NULL) {
                    *failed_index_out = i;
                }
                return false;
            }
        }
    }
    return true;
}

#endif
