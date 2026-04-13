#ifndef LINQU_UB_BRIDGE_H
#define LINQU_UB_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct LinquUbBridge LinquUbBridge;

LinquUbBridge *linqu_ub_bridge_new_from_yaml(const char *path);
void linqu_ub_bridge_free(LinquUbBridge *bridge);

int linqu_ub_bridge_register_endpoint(LinquUbBridge *bridge,
                                      uint16_t endpoint_id,
                                      uint32_t entity_id);

int linqu_ub_bridge_get_default_segment(LinquUbBridge *bridge,
                                        uint16_t endpoint_id,
                                        uint64_t *segment_out);

int linqu_ub_bridge_submit_slot(LinquUbBridge *bridge,
                                uint16_t endpoint_id,
                                const uint8_t *slot,
                                size_t slot_len);

int linqu_ub_bridge_ring_doorbell(LinquUbBridge *bridge,
                                  uint16_t endpoint_id,
                                  uint32_t max_batch,
                                  uint32_t *submitted_out,
                                  uint32_t *pending_out);

int linqu_ub_bridge_poll_completion(LinquUbBridge *bridge,
                                    uint16_t endpoint_id,
                                    uint8_t *slot_out,
                                    size_t slot_len);

#endif
