# SIM Decoder Implementation Summary

## Overview

This document summarizes the implementation of the Simulation Decoder (SIM_DEC) protocol for cross-node memory access in the QEMU/UB dual-node simulation environment.

## Architecture

The implementation follows a 4-layer architecture:

```
┌─────────────────────────────────────────────────────────────────────┐
│  User Space (OBMM)                                                  │
│  - prepare_import_memory()                                          │
│  - register_obmm_region()                                           │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│  Guest Kernel: Service Layer                                        │
│  - ub_sim_decoder_service.c                                         │
│  - Maintains map_entry list                                         │
│  - Validates requests (overlap, token_id)                           │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│  Guest Kernel: Control Adapter                                      │
│  - ub_sim_decoder_ctrl.c                                            │
│  - Protocol marshalling                                             │
│  - Message sync or ctrlq submission                                 │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│  QEMU Backend                                                       │
│  - ub_ubc.c (SIM_DEC protocol handlers)                             │
│  - hisi/ubc_msgq.c (SIM_DEC data-plane message dispatch)            │
│  - decoder_map_table (PA-indexed)                                   │
│  - DMA strict path hook for cross-node read/write                   │
└─────────────────────────────────────────────────────────────────────┘
```

## Files Created/Modified

### Guest Kernel (simulator/guest-linux/kernel_ub/drivers/ub/ubus/sim/)

| File | Description |
|------|-------------|
| `ub_sim_decoder.h` | Header with protocol definitions and structures |
| `ub_sim_decoder_service.c` | Service layer: map/unmap/sync/query operations |
| `ub_sim_decoder_ctrl.c` | Control adapter: message formatting |
| `ub_sim_decoder_backend.c` | Backend selection (SIM vs HW) |
| `ub_sim_decoder_main.c` | Module init/exit with OBMM integration hooks |
| `Makefile` | Build configuration |
| `../Makefile` | Updated to include sim/ subdirectory |
| `../Kconfig` | Added CONFIG_UB_UBUS_SIM_DECODER option |

### QEMU (simulator/vendor/qemu_8.2.0_ub/)

| File | Changes |
|------|---------|
| `hw/ub/ub_ubc.c` | Added SIM_DEC protocol implementation |
| `hw/ub/hisi/ubc_msgq.c` | Routed SIM_DEC control op + data-plane sub-op dispatch |
| `include/hw/ub/ub_ubc.h` | Added SIM_DEC data-plane payloads and handler declarations |

## SIM_DEC Protocol

### Message Header
```c
struct sim_dec_msg_hdr {
    u8  version;      /* Protocol version (1) */
    u8  opcode;       /* SIM_DEC_OP_* */
    u16 seq;          /* Sequence number */
    u16 status;       /* Response status */
    u16 payload_len;  /* Payload length */
};
```

### Opcodes
- `SIM_DEC_OP_MAP (0x01)` - Create decoder mapping
- `SIM_DEC_OP_UNMAP (0x02)` - Remove decoder mapping
- `SIM_DEC_OP_SYNC (0x03)` - Synchronize memory
- `SIM_DEC_OP_QUERY (0x04)` - Query mapping info

### Status Codes
- `SIM_DEC_STATUS_SUCCESS (0x00)`
- `SIM_DEC_STATUS_INVALID_PARAM (0x01)`
- `SIM_DEC_STATUS_RESOURCE_BUSY (0x02)`
- `SIM_DEC_STATUS_BACKEND_ERROR (0x03)`
- `SIM_DEC_STATUS_TIMEOUT (0x04)`
- `SIM_DEC_STATUS_NOT_SUPPORTED (0x05)`

## QEMU Backend Implementation

### Key Data Structures
```c
typedef struct SimDecMapEntry {
    uint64_t map_id;
    uint64_t local_pa;
    uint64_t size;
    uint64_t remote_uba;
    uint32_t token_id;
    uint32_t token_value;
    uint32_t scna, dcna;
    uint8_t  seid[16], deid[16];
    uint32_t upi, src_eid;
    bool     active;
    QTAILQ_ENTRY(SimDecMapEntry) next;
} SimDecMapEntry;

typedef struct SimDecoderState {
    BusControllerState *bcs;
    uint64_t next_map_id;
    QTAILQ_HEAD(, SimDecMapEntry) map_list;
    QemuMutex lock;
    bool enabled;
} SimDecoderState;
```

### API Functions
```c
/* Handle incoming SIM_DEC control message */
int ubc_handle_sim_dec_message(const uint8_t *data, uint32_t len,
                                uint8_t *resp, uint32_t *resp_len);

/* Lookup decoder entry by PA (called by UBC DMA strict path) */
int sim_dec_lookup_by_pa(uint64_t pa, uint64_t *remote_uba,
                         uint32_t *token_id, uint32_t *src_eid);
```

## Integration Points

### 1. OBMM Integration (Guest)
```c
/* In prepare_import_memory() flow:
 * 1. User calls import_memory_ioctl()
 * 2. OBMM calls prepare_import_memory()
 * 3. ub_sim_decoder_map() is called to create decoder config
 * 4. register_obmm_region() registers the memory
 */
```

### 2. Data-Path Integration (QEMU)
```c
/* In ubc_dma_read_ex()/ubc_dma_write_ex() strict DATA path:
 * 1. Check local PA hit via sim_dec_lookup_by_pa()
 * 2. If hit, route to cross-node SIM_DEC read/write message path
 * 3. Remote node executes strict DMA read/write with token-aware TID override
 * 4. READ uses request-response; WRITE is sent as payload message
 */
```

### 3. Message Channel Integration (Future)
```c
/* Control messages can be sent via:
 * Option A: message_sync_request() for blocking calls
 * Option B: ctrlq for async messaging
 */
```

## Build Instructions

### QEMU Build
```bash
cd simulator/vendor/qemu_8.2.0_ub/build
ninja -j8
```

### Guest Kernel Build
```bash
export KERNEL_BUILD_DIR=/path/to/kernel_build
export CROSS_COMPILE=aarch64-unknown-linux-gnu-
export ARCH=arm64
make -C $KERNEL_BUILD_DIR M=drivers/ub/ubus modules
```

## Testing Checklist

### Phase A: Framework (Completed)
- [x] Header files with protocol definitions
- [x] Service layer map/unmap/sync/query functions
- [x] Control adapter structure
- [x] QEMU backend decoder_map_table
- [x] SIM_DEC protocol handlers in QEMU

### Phase B: MAP/UNMAP + WRITE (Completed)
- [x] Message channel implementation (hisi private message path)
- [x] OBMM integration hooks (import/unimport callbacks)
- [x] PA lookup integration in DMA strict path
- [x] Cross-node WRITE via decoder mapping
- [x] Basic token_id validation

### Phase C: READ/SYNC (Mostly Completed)
- [x] READ request-response handling with decoder lookup
- [x] SYNC control op end-to-end request/response
- [ ] Memory drain/ownership transfer co-optimization (still relies on existing OBMM drain path)

### Phase D: Token Validation + RAS (Future)
- [ ] Token validation at map time
- [ ] Token validation at access time
- [ ] RAS error injection support
- [ ] Map conflict detection

## Known Limitations

1. **Token/Permission Depth**: Current access-time enforcement is token_id-based; token_value/permission bits are not fully modeled.
2. **RAS & Fault Injection**: Error injection and RAS observability are not yet complete.
3. **UAPI Explicitization**: `obmm_cmd_import` still uses `priv` payload for `remote_uba/token_value` (P0 mode), not explicit UAPI fields.

## References

- Design Doc: `sim_ub_cross_node_memory_access_design.md`
- Guest Decoder: `simulator/guest-linux/kernel_ub/drivers/ub/ubus/decoder.h`
- Guest Memory: `simulator/guest-linux/kernel_ub/drivers/ub/ubus/memory.h`
- QEMU UBC: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
- QEMU UMMU: `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ummu.c`
