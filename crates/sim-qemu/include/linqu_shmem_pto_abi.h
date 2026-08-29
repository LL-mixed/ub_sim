#ifndef LINQU_SHMEM_PTO_ABI_H
#define LINQU_SHMEM_PTO_ABI_H

#include <stddef.h>
#include <stdint.h>

#define LINGQU_PTO_DISPATCH_ABI_V2 2u
#define LINGQU_SHMEM_MEMREF_ABI_V1 1u
#define LINGQU_PTO_SCALAR_ABI_V1 1u
#define PTO_SIM_UB_GM_ACCESS_ABI_V1 1u

/*
 * Existing Lingqu guest descriptors use byte 0 as a transport tag. Tag 10
 * carries an IoOpcode::Dispatch v2 control-table reference; it does not add a
 * new semantic NPU opcode. Multi-byte fields are little-endian byte arrays so
 * the 64-byte wire layout has no host-ABI padding or alignment dependency.
 */
#define LINGQU_PTO_DISPATCH_SLOT_TAG_V2 10u
#define LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET 1u
#define LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET 9u
#define LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET 17u

#define LINGQU_PTO_MAX_MEMREFS 256u
#define LINGQU_PTO_MAX_SCALARS 128u
#define LINGQU_PTO_MAX_RANK 5u
#define LINGQU_PTO_DTYPE_MAX 14u
#define LINGQU_PTO_CNA_MAX 0x00ffffffu

/*
 * The simulator adaptor turns an active OBMM map registration into one opaque
 * 64-bit reference for the dispatch wire ABI.  The low byte identifies one of
 * the 64 endpoint map slots and the remaining 56 bits preserve its generation.
 * Applications receive the encoded value from the Lingqu shmem adaptor and
 * must not interpret either component.
 */
#define LINGQU_PTO_OBMM_MAP_ID_BITS 8u
#define LINGQU_PTO_OBMM_MAP_ID_MASK UINT64_C(0xff)
#define LINGQU_PTO_OBMM_MAP_GENERATION_MAX \
    (UINT64_MAX >> LINGQU_PTO_OBMM_MAP_ID_BITS)

static inline uint64_t lingqu_pto_obmm_mapping_ref_encode(
    uint64_t map_id, uint64_t map_generation)
{
    if (map_id == 0 || map_id > LINGQU_PTO_OBMM_MAP_ID_MASK ||
        map_generation == 0 ||
        map_generation > LINGQU_PTO_OBMM_MAP_GENERATION_MAX) {
        return 0;
    }
    return (map_generation << LINGQU_PTO_OBMM_MAP_ID_BITS) | map_id;
}

static inline uint64_t lingqu_pto_obmm_mapping_ref_map_id(
    uint64_t mapping_ref)
{
    return mapping_ref & LINGQU_PTO_OBMM_MAP_ID_MASK;
}

static inline uint64_t lingqu_pto_obmm_mapping_ref_generation(
    uint64_t mapping_ref)
{
    return mapping_ref >> LINGQU_PTO_OBMM_MAP_ID_BITS;
}

enum LingquPtoMemrefRole {
    LINGQU_PTO_MEMREF_INPUT = 1,
    LINGQU_PTO_MEMREF_OUTPUT = 2,
    LINGQU_PTO_MEMREF_INOUT = 3,
};

enum LingquPtoUbGmAccess {
    LINGQU_PTO_UB_GM_READ = 1u << 0,
    LINGQU_PTO_UB_GM_WRITE = 1u << 1,
    LINGQU_PTO_UB_GM_READ_WRITE =
        LINGQU_PTO_UB_GM_READ | LINGQU_PTO_UB_GM_WRITE,
};

enum LingquPtoUbGmError {
    LINGQU_PTO_UB_GM_OK = 0,
    LINGQU_PTO_UB_GM_UNSUPPORTED_CALLABLE = 1,
    LINGQU_PTO_UB_GM_BAD_CONTROL_TABLE = 2,
    LINGQU_PTO_UB_GM_BAD_MEMREF = 3,
    LINGQU_PTO_UB_GM_UNBOUND = 4,
    LINGQU_PTO_UB_GM_ACCESS_DENIED = 5,
    LINGQU_PTO_UB_GM_AUTHORIZATION_TIMEOUT = 6,
    LINGQU_PTO_UB_GM_CALLBACK_FAILED = 7,
    LINGQU_PTO_UB_GM_EXECUTION_FAILED = 8,
};

#define LINGQU_PTO_UB_GM_CODE_UNSUPPORTED_CALLABLE \
    "pto_ub_gm_unsupported_callable"
#define LINGQU_PTO_UB_GM_CODE_BAD_CONTROL_TABLE \
    "pto_ub_gm_bad_control_table"
#define LINGQU_PTO_UB_GM_CODE_BAD_MEMREF "pto_ub_gm_bad_memref"
#define LINGQU_PTO_UB_GM_CODE_UNBOUND "pto_ub_gm_unbound"
#define LINGQU_PTO_UB_GM_CODE_ACCESS_DENIED "pto_ub_gm_access_denied"
#define LINGQU_PTO_UB_GM_CODE_AUTHORIZATION_TIMEOUT \
    "pto_ub_gm_authorization_timeout"
#define LINGQU_PTO_UB_GM_CODE_CALLBACK_FAILED "pto_ub_gm_callback_failed"
#define LINGQU_PTO_UB_GM_CODE_EXECUTION_FAILED "pto_ub_gm_execution_failed"

typedef struct LingquPtoDispatchSlotV2 {
    uint8_t descriptor_tag;
    uint8_t op_id_le[8];
    uint8_t control_table_iova_le[8];
    uint8_t reserved[47];
} LingquPtoDispatchSlotV2;

typedef struct LingquPtoDispatchControlV2 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t request_id;
    uint64_t callable_id;
    uint32_t memref_count;
    uint32_t scalar_count;
    uint64_t memref_table_iova;
    uint64_t scalar_table_iova;
    uint64_t artifact_fingerprint;
    uint32_t metadata_crc32;
    uint32_t requester_cna;
} LingquPtoDispatchControlV2;

typedef struct LingquShmemMemrefV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t opaque_mapping_ref;
    uint64_t ub_gm_addr;
    uint64_t byte_offset;
    uint64_t byte_length;
    uint64_t shape_table_iova;
    uint64_t stride_table_iova;
    uint32_t arg_index;
    uint32_t rank;
    uint16_t dtype;
    uint8_t role;
    uint8_t access;
    uint32_t flags;
    uint32_t reserved0;
    uint32_t reserved1;
} LingquShmemMemrefV1;

typedef struct LingquPtoScalarV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint32_t arg_index;
    uint16_t dtype;
    uint16_t flags;
    uint64_t value;
} LingquPtoScalarV1;

typedef struct PtoSimUbGmBindingV1 {
    uint64_t request_id;
    uint64_t binding_id;
    uint64_t aperture_base;
    uint64_t aperture_length;
    uint64_t ub_gm_base;
    uint64_t mapped_length;
    uint32_t access;
    uint32_t flags;
    uint64_t backend_cookie;
} PtoSimUbGmBindingV1;

/*
 * QEMU-to-Rust bridge object. QEMU has already DMA-read shape/stride tables
 * and authorized the mapping before this object crosses the bridge.
 */
typedef struct PtoSimUbGmAuthorizedMemrefV1 {
    LingquShmemMemrefV1 memref;
    PtoSimUbGmBindingV1 binding;
    uint32_t shape[LINGQU_PTO_MAX_RANK];
    uint32_t strides[LINGQU_PTO_MAX_RANK];
    uint64_t reserved;
} PtoSimUbGmAuthorizedMemrefV1;

typedef int (*PtoSimUbGmReadV1)(void *qemu_context,
                                uint64_t request_id,
                                uint64_t binding_id,
                                uint64_t ub_gm_addr,
                                void *dst,
                                uint64_t length);

typedef int (*PtoSimUbGmWriteV1)(void *qemu_context,
                                 uint64_t request_id,
                                 uint64_t binding_id,
                                 uint64_t ub_gm_addr,
                                 const void *src,
                                 uint64_t length);

typedef int (*PtoSimUbGmFenceV1)(void *qemu_context,
                                 uint64_t request_id,
                                 uint64_t binding_id,
                                 uint64_t ub_gm_addr,
                                 uint64_t length,
                                 uint32_t flags);

typedef struct PtoSimUbGmAccessOpsV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    PtoSimUbGmReadV1 read;
    PtoSimUbGmWriteV1 write;
    PtoSimUbGmFenceV1 fence;
} PtoSimUbGmAccessOpsV1;

typedef struct LingquPtoUbGmCountersV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t request_id;
    uint64_t h2d_bytes;
    uint64_t d2h_bytes;
    uint64_t segment_payload_staging_bytes;
    uint64_t pto_ub_gm_read_bytes;
    uint64_t pto_ub_gm_write_bytes;
    uint64_t qemu_ub_gm_load_bytes;
    uint64_t qemu_ub_gm_store_bytes;
    uint64_t qemu_ub_gm_fence_count;
    uint32_t producer_verify;
    uint32_t last_error;
    uint64_t reserved;
} LingquPtoUbGmCountersV1;

#if defined(__cplusplus)
#define LINQU_PTO_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define LINQU_PTO_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

LINQU_PTO_STATIC_ASSERT(sizeof(void *) == 8, "Lingqu PTO ABI requires 64-bit pointers");
LINQU_PTO_STATIC_ASSERT(sizeof(LingquPtoDispatchSlotV2) == 64,
                        "LingquPtoDispatchSlotV2 must stay 64 bytes");
LINQU_PTO_STATIC_ASSERT(offsetof(LingquPtoDispatchSlotV2, op_id_le) ==
                            LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET,
                        "Lingqu PTO dispatch slot op_id offset changed");
LINQU_PTO_STATIC_ASSERT(offsetof(LingquPtoDispatchSlotV2,
                                 control_table_iova_le) ==
                            LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET,
                        "Lingqu PTO dispatch slot control IOVA offset changed");
LINQU_PTO_STATIC_ASSERT(offsetof(LingquPtoDispatchSlotV2, reserved) ==
                            LINGQU_PTO_DISPATCH_SLOT_RESERVED_OFFSET,
                        "Lingqu PTO dispatch slot reserved offset changed");
LINQU_PTO_STATIC_ASSERT(sizeof(LingquPtoDispatchControlV2) == 64,
                        "LingquPtoDispatchControlV2 must stay 64 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(LingquShmemMemrefV1) == 80,
                        "LingquShmemMemrefV1 must stay 80 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(LingquPtoScalarV1) == 24,
                        "LingquPtoScalarV1 must stay 24 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(PtoSimUbGmBindingV1) == 64,
                        "PtoSimUbGmBindingV1 must stay 64 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(PtoSimUbGmAuthorizedMemrefV1) == 192,
                        "PtoSimUbGmAuthorizedMemrefV1 must stay 192 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(PtoSimUbGmAccessOpsV1) == 32,
                        "PtoSimUbGmAccessOpsV1 must stay 32 bytes");
LINQU_PTO_STATIC_ASSERT(sizeof(LingquPtoUbGmCountersV1) == 96,
                        "LingquPtoUbGmCountersV1 must stay 96 bytes");

#undef LINQU_PTO_STATIC_ASSERT

#endif
