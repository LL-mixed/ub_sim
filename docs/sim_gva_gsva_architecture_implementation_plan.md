# GVA-GSVA Architecture Design and Implementation Plan

This document is the canonical GVA-GSVA plan for `ub_sim`.

It merges and supersedes:

- `docs/sim_gva_gsva_final_architecture_execution_plan.md`
- `docs/sim_gva_gsva_implementation_spec.md`

The document has two parts:

- Part I: architecture design.
- Part II: implementation plan.

The goal is to make implementation decisions explicit enough that coding can proceed without interpreting intent from multiple documents.

## Part I: Architecture Design

## 1. Goal

The final default guest access path must be:

```text
guest access
  -> ARM MMU / page-table-visible GSVA metadata
  -> QEMU GVA/GSVA route lookup
  -> GSVA-keyed coherence
  -> UB Link / OBMM data backend
```

The system must satisfy these product-level properties:

- Strict GSVA mappings preserve `user_va == uba == home_va`.
- ARM MMU is the default access path.
- `SIM_DEC` remains a compatibility and diagnostic path.
- `SIM_GVA_TCG` remains a transition/debug path, not the final default data path.
- Coherence identity is GSVA semantic identity, not PA identity.
- Segment retire/reuse is atomic from the user's perspective: stale mappings are not silently reused.

## 2. Non-goals

- Do not precisely model real ARM cache microarchitecture in the first version.
- Do not replace the existing OBMM directory MESI data layer.
- Do not remove legacy `SIM_DEC_OP_MAP`, `SIM_DEC_OP_UNMAP`, `SIM_DEC_OP_SYNC`, or `SIM_DEC_OP_QUERY`.
- Do not migrate every existing demo to ARM MMU mode in one step.
- Do not pass tests by disabling GSVA key validation, epoch validation, token validation, or coherence ACK.

## 3. Current baseline

The repository already has useful foundations:

- GVA Manager and GSVA segment lifecycle primitives exist.
- Guest kernel has `MAP_GSVA`, aperture registration, aperture lookup, cleanup, and overlap protection.
- QEMU has `SIM_DEC_OP_GVA_MAP`.
- `SIM_GVA_TCG` has direct validation paths for write/read, unmap-fault, and statistics.
- OBMM directory MESI has multi-node validation and should remain the data-layer coherence substrate.

The missing pieces are:

- The default path is not yet ARM MMU.
- Coherence identity is still too close to PA-MESI state.
- Segment lifecycle is not transactionally bound to GSVA route and coherence state.
- There is no stable GSVA metadata ABI for map/unmap/event/query.
- CLI, logs, and tests do not yet form a complete 2/4/8-node acceptance matrix.

## 4. Architecture layers

Implementation must keep three layers separate.

```text
GSVA semantic layer
  key identity
  epoch and stale rejection
  token and permission validation
  GSVA coherence ownership
  segment retire/reuse transaction

GVA route layer
  ma_table: {vmid, asid, uba_range} -> {dcna, tid, upi, p_tag, token}
  mp_table: {p_tag} -> {ubc_port, link, lane}
  route lookup from GSVA UBA to UB Link target

OBMM/PA data layer
  backing memory
  OBMM import/export lifecycle
  directory MESI line cache
  persistent point writeback/fence
```

Layering rules:

- GSVA semantic layer decides whether an access is valid.
- GVA route layer decides where a valid access goes.
- OBMM/PA layer moves bytes and enforces line-level data coherence.
- PA-MESI must only run after GSVA validation succeeds.
- `ub_ubc.c` may integrate the layers, but it must not own GSVA state-machine logic.

## 5. Core invariants

These invariants are mandatory:

- Strict GSVA requires `user_va == uba == home_va`.
- Strict GSVA rejects relocated `mmap`.
- Strict GSVA rejects `mmap(NULL, ...)`.
- Strict GSVA rejects missing QEMU GSVA feature support.
- Strict GSVA rejects mapping outside a registered GSVA aperture.
- Legacy `SIM_DEC_OP_MAP` payload must remain wire-compatible.
- Token is permission state, not key identity.
- Cache policy is key identity.
- In-place cache policy mutation is forbidden.
- Segment reuse cannot commit before retire ACK and PA-MESI fence/writeback complete.
- GSVA stats must remain separate from OBMM PA-MESI stats.

## 6. Protocol freeze decisions

The following decisions are closed for the first implementation.

### 6.1 Numeric opcodes

Existing opcodes are already occupied:

```c
#define SIM_DEC_OP_MAP                    0x01
#define SIM_DEC_OP_UNMAP                  0x02
#define SIM_DEC_OP_SYNC                   0x03
#define SIM_DEC_OP_QUERY                  0x04
#define SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH 0x05
#define SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP  0x06
#define SIM_DEC_OP_GVA_MAP                0x07
#define SIM_DEC_OP_COH_FENCE              0x08
```

New GSVA opcodes start at `0x09`:

```c
#define SIM_DEC_OP_GSVA_MAP_V1    0x09
#define SIM_DEC_OP_GSVA_UNMAP_V1  0x0a
#define SIM_DEC_OP_GSVA_EVENT_V1  0x0b
#define SIM_DEC_OP_GSVA_QUERY_V1  0x0c
```

Do not reuse `SIM_DEC_OP_GVA_MAP` for GSVA. GVA map is a transition sideband. GSVA requires a versioned ABI.

### 6.2 UAPI header location

Create the stable UAPI header:

```text
guest-linux/kernel_ub/include/uapi/ub/gsva.h
```

Mirror constants into existing internal or app headers only when required by the current build structure:

```text
guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder.h
guest-linux/aarch64/common/obmm_common.h
```

`gsva.h` is the source of truth for new ABI definitions.

### 6.3 VMID and ASID v1 rule

The first implementation uses:

```text
vmid = 0
asid = 0
```

Reason:

- Current acceptance scope is 2/4/8-node single-VM simulation.
- Linux hardware ASID plumbing is not required for the first correctness milestone.
- Using unstable process-derived ASID would create false key splits and harder-to-debug stale behavior.

User impact:

- All mappings in the first version share one VM/address-space domain.
- Per-process isolation with the same `home_va` is a future extension.

Future rule:

- Multi-VM support must set `vmid`.
- Per-process GSVA isolation must set `asid`.
- Enabling non-zero `vmid/asid` requires new acceptance tests.

### 6.4 Segment identity v1 rule

`segment_id` is an opaque `uint64_t` allocated by the segment home manager.

For deterministic simulator allocation, use:

```text
segment_id = (home_cna << 48) | local_segment_counter
```

Rules:

- `home_cna` uses the low 16 bits.
- `local_segment_counter` uses the low 48 bits and starts at 1.
- Counter value 0 is invalid.
- The home manager must not reuse a `segment_id` within one run.
- Segment reuse uses a higher `epoch`, not the same `(segment_id, epoch)`.

User impact:

- Segment IDs are stable in logs and easy to trace back to the home node.
- Reuse is explicit through epoch, not hidden by ID recycling.

### 6.5 ARM MMU metadata v1 rule

The first `arm_mmu` implementation uses a QEMU side table, not PTE bit encoding.

Reason:

- It avoids consuming architecture-specific PTE bits before the simulator semantics stabilize.
- It lets the existing GVA route metadata become the initial source of `pte_offset`.
- It lowers risk while still moving the default path into ARM MMU/TLB lookup.

Future rule:

- PTE encoding may be added later as an optimization or realism improvement.
- The side-table ABI must remain valid after PTE encoding exists.

## 7. GSVA identity model

The original key fields are kept, but lookup semantics are split into three concepts to avoid stale epoch ambiguity.

### 7.1 Base identity

The base identity identifies the logical GSVA object independent of generation.

```text
segment_id
home_va
vmid
asid
pte_offset
p_tag
cache_policy
```

### 7.2 Active generation

The active generation is:

```text
base identity + epoch
```

`epoch` is monotonic per segment lifecycle.

### 7.3 Access containment

`size` is not part of identity.

It is used for containment validation:

```text
access_va >= home_va
access_va + access_len <= home_va + size
```

Invalid containment returns:

```text
GSVA_ERR_KEY_MISMATCH
```

### 7.4 Lookup order

All access validation follows this order:

```text
1. route lookup by {vmid, asid, access_va}
2. identify base GSVA object
3. validate containment
4. compare supplied epoch with active epoch
5. validate token
6. validate access permissions
7. acquire GSVA coherence permission
8. call PA-MESI data operation
```

Epoch mismatch rules:

- If base identity exists and supplied epoch is older than active epoch, return `GSVA_ERR_STALE_EPOCH`.
- If base identity exists and supplied epoch is newer than active epoch, return `GSVA_ERR_STALE_EPOCH`.
- If base identity is retired, return `GSVA_ERR_SEGMENT_RETIRED`.
- If no base identity exists, return `GSVA_ERR_ROUTE_MISSING`.

Reason:

- Route miss and stale epoch must be distinguishable in logs.
- Old mappings must fail with a semantic stale error, not an accidental missing-route error.

## 8. GSVA key ABI

### 8.1 Key struct

```c
struct gsva_key_v1 {
    uint32_t version;       /* must be 1 */
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t vmid;
    uint64_t asid;
    uint64_t pte_offset;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint64_t epoch;
};
```

ABI rules:

- All fields use fixed-width integer types.
- Wire encoding is little-endian.
- Payload length must be checked before dereferencing.
- `version` must equal 1.
- `flags` must be zero unless explicitly defined.
- Unknown non-zero required flags return `GSVA_ERR_BAD_VERSION`.

### 8.2 Field semantics

- `segment_id`: allocation identity from the home GVA Manager.
- `home_va`: GSVA base. In strict GSVA, `user_va == uba == home_va`.
- `size`: segment byte size.
- `vmid`: guest VM context. V1 uses 0.
- `asid`: guest address-space context. V1 uses 0.
- `pte_offset`: metadata offset from the page-table-visible route side table. Strict segment-object V1 uses 0.
- `p_tag`: route tag used by NoC `mp_table`.
- `cache_policy`: route/cache policy. `DIRECTORY_MESI` remains value 4.
- `epoch`: segment lifecycle generation.

### 8.3 Cache policy

The existing directory MESI policy remains:

```c
#define OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI 4
```

Changing `cache_policy` creates a new key identity.

Required sequence:

```text
old key: revoke + invalidate + drain + unmap
new key: map with new cache_policy
```

In-place mutation is forbidden.

## 9. Token and permission model

`token_id` and `token_value` are permissions, not key identity.

### 9.1 V1 token source

V1 reuses the existing OBMM export/import token model.

Token fields already exist in the current flow:

```text
export metadata: token_id
import private data: token_value
route metadata: token_id, token_value, access_flags
```

Initial token assignment:

```text
1. exporter creates or publishes an OBMM export record
2. exporter allocates token_id for that exported memory object
3. exporter generates token_value for the initial lease
4. bootstrap/publish metadata exposes token_id, remote_uba, size, export_cna
5. importer receives token_value through the existing import private data path
6. importer includes token_id/token_value in SIM_DEC_OP_GSVA_MAP_V1
7. QEMU stores token metadata in gsva_route, not in gsva_key identity
```

V1 token sizes:

```c
uint32_t token_id;
uint32_t token_value;
```

The GSVA ABI structs may carry `uint64_t` token fields for alignment and future expansion, but V1 validation must reject values that do not fit in 32 bits.

### 9.2 Valid token definition

A V1 token is valid only when it matches an active route lease.

The authoritative token state is:

```c
enum gsva_token_state_v1 {
    GSVA_TOKEN_INVALID = 0,
    GSVA_TOKEN_ACTIVE = 1,
    GSVA_TOKEN_REVOKING = 2,
    GSVA_TOKEN_REVOKED = 3,
};

struct gsva_token_v1 {
    uint32_t token_id;
    uint32_t token_value;
    uint32_t access_flags;
    uint32_t flags;
    uint64_t lease_epoch;
    uint64_t allowed_cna_bitmap;
    enum gsva_token_state_v1 state;
};
```

V1 valid-token predicate:

```text
valid_token(route, requester_cna, token_id, token_value, access_type) is true iff:
  route exists and route is active
  route base identity matches the requested GSVA object
  route segment epoch equals gsva_key_v1.epoch
  route.token.state == GSVA_TOKEN_ACTIVE
  route.token.token_id != 0
  route.token.token_value != 0
  supplied token_id == route.token.token_id
  supplied token_value == route.token.token_value
  requester_cna is allowed by allowed_cna_bitmap, or allowed_cna_bitmap == 0
  access_type is permitted by route.token.access_flags
```

Strict GSVA V1 always requires token value validation. There is no "token id only" mode for strict GSVA.

Runtime CPU access rule:

- Guest load/store instructions do not carry token fields.
- `SIM_DEC_OP_GSVA_MAP_V1` installs the token into `gsva_route`.
- ARM MMU mode copies token metadata into the GSVA TLB side table when the TLB entry is installed.
- ReadAcquire/WriteAcquire validates the route lease and the TLB metadata against the active route lease before changing coherence state.

Validation failure must not modify GSVA coherence state and must not call PA-MESI.

Required validation helper:

```c
int gsva_route_validate_token(const struct gsva_route_entry *route,
                              uint32_t requester_cna,
                              uint32_t token_id,
                              uint32_t token_value,
                              uint32_t access_type);
```

Return mapping:

```text
success                         -> GSVA_OK
missing or inactive route        -> GSVA_ERR_ROUTE_MISSING
epoch mismatch                   -> GSVA_ERR_STALE_EPOCH
token state not ACTIVE           -> GSVA_ERR_TOKEN_DENIED
token_id mismatch                -> GSVA_ERR_TOKEN_DENIED
token_value mismatch             -> GSVA_ERR_TOKEN_DENIED
requester_cna not allowed        -> GSVA_ERR_TOKEN_DENIED
read/write access not permitted  -> GSVA_ERR_TOKEN_DENIED
```

Milestone 3 requirement:

```text
ReadAcquire:
  validate route/key/epoch
  validate token lease for read
  acquire GSVA shared state
  call PA-MESI read

WriteAcquire:
  validate route/key/epoch
  validate token lease for write
  acquire GSVA exclusive/modified state
  call PA-MESI write
```

### 9.3 Token ownership

The exporter/home manager is the authority for token creation and rotation.

Rules:

- Importers never mint tokens.
- QEMU validates tokens but does not invent them.
- `gva_manager` may display token metadata but must not log raw `token_value` unless a diagnostic flag is explicitly enabled.
- `token_id == 0` is invalid for a protected GSVA mapping.
- `token_value == 0` is invalid when token validation is enabled.

User impact:

- A stale or wrong import credential fails deterministically with `GSVA_ERR_TOKEN_DENIED`.
- Token rotation does not change address identity, so existing stale-address diagnostics remain meaningful.

### 9.4 Token table

QEMU keeps token state in `gsva_route`.

Minimum route token state:

```c
struct gsva_token_v1 {
    uint32_t token_id;
    uint32_t token_value;
    uint32_t access_flags;
    uint32_t flags;
    uint64_t lease_epoch;
};
```

`lease_epoch` is distinct from `gsva_key_v1.epoch`.

Rules:

- `gsva_key_v1.epoch` tracks segment lifecycle.
- `lease_epoch` tracks permission rotation.
- Token rotation increments `lease_epoch` and emits `GSVA_EVENT_TOKEN_CHANGE`.
- Token rotation does not create a new GSVA key.

### 9.5 Validation order

Access validation order:

```text
lookup base identity
validate containment
validate epoch
validate token
validate read/write permission
acquire coherence state
```

Token validation details:

```text
1. route token_id must match supplied token_id
2. route token_value must match supplied token_value
3. access_flags must permit the requested read/write operation
4. token lease must not be revoked
```

Failure mapping:

```text
token_id mismatch       -> GSVA_ERR_TOKEN_DENIED
token_value mismatch    -> GSVA_ERR_TOKEN_DENIED
revoked lease           -> GSVA_ERR_TOKEN_DENIED
write to read-only map  -> GSVA_ERR_TOKEN_DENIED
```

### 9.6 Token change sequence

Changing a token emits:

```text
GSVA_EVENT_TOKEN_CHANGE
```

Required sequence:

```text
1. home manager generates new token_value for existing token_id
2. home manager emits GSVA_EVENT_TOKEN_CHANGE(base identity, lease_epoch + 1)
3. QEMU marks old lease pending revoke
4. QEMU sends GSVA token revoke to current holders
5. holders drop local token cache and flush GSVA TLB entries for the range
6. holders ACK token revoke
7. QEMU commits new token_value and lease_epoch
8. new accesses must present the new token_value
```

Timeout behavior:

```text
token-change timeout -> GSVA_ERR_COH_TIMEOUT
old token access     -> GSVA_ERR_TOKEN_DENIED
new maps while pending token change -> GSVA_ERR_COH_PENDING
```

### 9.7 Token tests

Required CLI modes:

```text
gsva_coh_test --mode token_valid
gsva_coh_test --mode token_denied
gsva_coh_test --mode token_rotate
```

Required assertions:

```text
valid token succeeds
wrong token_value fails with GSVA_ERR_TOKEN_DENIED
old token after rotation fails with GSVA_ERR_TOKEN_DENIED
token rotation does not change gsva_key identity
```

## 10. Guest/QEMU ABI

### 10.1 Legacy ABI rule

`SIM_DEC_OP_MAP` v1 must remain wire-compatible.

Do not append GSVA fields to the legacy packed map payload.

### 10.2 GSVA map payload

```c
struct sim_dec_gsva_map_v1 {
    uint32_t version;
    uint32_t flags;
    struct gsva_key_v1 key;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint64_t token_id;
    uint64_t token_value;
    uint32_t source;
    uint32_t address_profile;
};
```

Rules:

- `version` must equal 1.
- `source` identifies the caller path.
- `address_profile` identifies strict or compatibility behavior.
- In strict GSVA, `local_va == remote_uba == key.home_va`.
- `local_pa` may be 0 for query/dry-run paths that do not yet bind a local PA window.

Source values:

```c
#define GSVA_SOURCE_IMPORT_PA_WINDOW 1
#define GSVA_SOURCE_ARM_MMU          2
#define GSVA_SOURCE_QUERY_DRY_RUN    3
```

Address profile values:

```c
#define GSVA_ADDRESS_PROFILE_LEGACY_RELOCATABLE 0
#define GSVA_ADDRESS_PROFILE_STRICT_GSVA        1
#define GSVA_ADDRESS_PROFILE_COMPAT_GSVA        2
```

Strict mode only accepts:

```text
GSVA_ADDRESS_PROFILE_STRICT_GSVA
```

### 10.3 GSVA event payload

```c
enum gsva_event_type {
    GSVA_EVENT_MAP = 1,
    GSVA_EVENT_MAP_UPDATE = 2,
    GSVA_EVENT_UNMAP = 3,
    GSVA_EVENT_SEGMENT_RETIRE = 4,
    GSVA_EVENT_SEGMENT_REUSE = 5,
    GSVA_EVENT_TOKEN_CHANGE = 6,
    GSVA_EVENT_CACHE_POLICY_CHANGE = 7,
    GSVA_EVENT_TLB_FLUSH = 8,
};

struct sim_dec_gsva_event_v1 {
    uint32_t version;
    uint32_t type;
    struct gsva_key_v1 key;
    uint64_t new_epoch;
    uint64_t flags;
};
```

Event rules:

- `GSVA_EVENT_UNMAP` removes route visibility after coherence release succeeds.
- `GSVA_EVENT_SEGMENT_RETIRE` starts a retire transaction.
- `GSVA_EVENT_SEGMENT_REUSE` is accepted only after the old key is retired.
- `GSVA_EVENT_CACHE_POLICY_CHANGE` must be implemented as old-key revoke plus new-key map.
- `GSVA_EVENT_TLB_FLUSH` must include the key range that was flushed.

### 10.4 GSVA query payload

`SIM_DEC_OP_GSVA_QUERY_V1` supports capability query and object query.

Request:

```c
struct sim_dec_gsva_query_v1 {
    uint32_t version;
    uint32_t query_type;
    struct gsva_key_v1 key;
};
```

Query types:

```c
#define GSVA_QUERY_CAPS       1
#define GSVA_QUERY_ROUTE      2
#define GSVA_QUERY_COHERENCE  3
#define GSVA_QUERY_SEGMENT    4
```

Capability response:

```c
struct sim_dec_gsva_caps_v1 {
    uint32_t version;
    uint32_t flags;
    uint32_t max_nodes;
    uint32_t supported_cache_policies;
    uint32_t supported_modes;
    uint32_t reserved;
};
```

Capability flags:

```c
#define GSVA_CAP_STRICT_ADDRESS_IDENTITY  (1u << 0)
#define GSVA_CAP_ROUTE_LAYER              (1u << 1)
#define GSVA_CAP_COHERENCE_LAYER          (1u << 2)
#define GSVA_CAP_ARM_MMU_MODE             (1u << 3)
#define GSVA_CAP_RETIRE_REUSE_TXN         (1u << 4)
```

Strict GSVA must fail if required capability bits are missing.

## 11. Error names

Stable error names must appear in logs, query output, and acceptance verdicts.

```text
GSVA_OK
GSVA_ERR_BAD_VERSION
GSVA_ERR_KEY_MISMATCH
GSVA_ERR_STALE_EPOCH
GSVA_ERR_TOKEN_DENIED
GSVA_ERR_ROUTE_MISSING
GSVA_ERR_COH_PENDING
GSVA_ERR_COH_TIMEOUT
GSVA_ERR_TLB_STALE
GSVA_ERR_SEGMENT_RETIRED
GSVA_ERR_UNSUPPORTED_POLICY
GSVA_ERR_STRICT_ADDRESS
GSVA_ERR_FEATURE_MISSING
```

Return-code mapping:

- `GSVA_ERR_BAD_VERSION`: malformed version, short payload, unsupported required flag.
- `GSVA_ERR_KEY_MISMATCH`: identity or range mismatch.
- `GSVA_ERR_STALE_EPOCH`: request epoch differs from active epoch for a known base identity.
- `GSVA_ERR_TOKEN_DENIED`: token validation failed.
- `GSVA_ERR_ROUTE_MISSING`: route lookup failed for the requested VA/context.
- `GSVA_ERR_COH_PENDING`: another operation is pending for the same object.
- `GSVA_ERR_COH_TIMEOUT`: pending coherence operation timed out.
- `GSVA_ERR_TLB_STALE`: TLB metadata exists but no longer matches the active key.
- `GSVA_ERR_SEGMENT_RETIRED`: retired object was accessed.
- `GSVA_ERR_UNSUPPORTED_POLICY`: cache policy is unknown or disabled.
- `GSVA_ERR_STRICT_ADDRESS`: strict identity rule failed.
- `GSVA_ERR_FEATURE_MISSING`: strict GSVA requested but QEMU lacks required capability.

## 12. Mode and configuration model

User-facing scripts use environment variables:

```text
GSVA_MODE=legacy_sim_dec
GSVA_MODE=sim_gva_tcg
GSVA_MODE=arm_mmu
GSVA_STRICT=0|1
GSVA_COH_TIMEOUT_MS=5000
```

Internal QEMU mode names are:

```text
gsva.mode=legacy_sim_dec
gsva.mode=sim_gva_tcg
gsva.mode=arm_mmu
gsva.strict=0|1
```

Rules:

- Launch scripts are responsible for translating `GSVA_MODE` into the QEMU setting.
- During migration, QEMU may read `GSVA_MODE` directly as a compatibility shim.
- `SIM_GVA_TCG=1` remains accepted only as a compatibility alias for `GSVA_MODE=sim_gva_tcg`.
- Final default is `GSVA_MODE=arm_mmu` and `GSVA_STRICT=1`.
- Development default may remain `GSVA_MODE=sim_gva_tcg` until Milestone 5 is accepted.

Strict mode requirements:

- `user_va == uba == home_va`.
- No relocated mmap fallback.
- No fallback to legacy map if QEMU lacks GSVA support.
- Missing GSVA metadata is a hard failure.

## 13. QEMU architecture

### 13.1 New modules

Add:

```text
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.h
```

Update QEMU build metadata:

```text
vendor/qemu_8.2.0_ub/hw/ub/meson.build
```

Keep:

```text
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.h
```

### 13.2 Module ownership

`gsva_key` owns:

- ABI validation.
- Key/base-identity comparison.
- Containment checks.
- Stable error-name helpers.

`gsva_route` owns:

- GSVA map/unmap/query route tables.
- VA range lookup.
- Token metadata attached to a route.
- Retired-object tombstones needed to distinguish stale epoch from route missing.

`gsva_coherence` owns:

- GSVA object state.
- ReadAcquire and WriteAcquire.
- revoke/invalidate/downgrade/writeback/fence orchestration.
- pending sequence handling.
- timeout terminal state.

`gsva_stats` owns:

- GSVA stats counters.
- route stats counters.
- formatted diagnostics export.

`ub_ubc.c` owns:

- Opcode dispatch.
- Integration with existing SIM_DEC transport.
- Calling route/coherence/data-layer functions.
- Logging run-visible events.

`ub_ubc.c` must not own:

- GSVA key equality.
- GSVA coherence transitions.
- Retire/reuse transaction state.

### 13.3 ARM MMU hook v1

The first ARM MMU path uses the existing ARM TLB fill hook that currently hosts the `SIM_GVA_TCG` probe.

Primary QEMU entry point:

```text
vendor/qemu_8.2.0_ub/target/arm/tcg/tlb_helper.c
arm_cpu_tlb_fill()
```

Current transition behavior:

```text
arm_cpu_tlb_fill()
  -> get_phys_addr()
  -> sim_dec_gva_tcg_translate()
  -> rewrite res.f.phys_addr to local CPU-window PA
  -> tlb_set_page_full()
```

Final GSVA behavior:

```text
arm_cpu_tlb_fill()
  -> get_phys_addr()
  -> gsva_arm_mmu_translate()
  -> validate GSVA route/key/token/coherence for the requested access
  -> rewrite res.f.phys_addr to local CPU-window PA or generated GSVA backend PA
  -> attach GSVA TLB metadata in side table
  -> tlb_set_page_full()
```

Do not add a new out-of-band CPU access path for `arm_mmu`. The existing ARM MMU/TLB fill path is the integration point.

Required entry point:

```c
bool gsva_arm_mmu_translate(CPUState *cs,
                            uint64_t va,
                            bool is_write,
                            int mmu_idx,
                            struct GSVAArmMmuResult *out);
```

Lookup result:

```c
struct GSVAArmMmuResult {
    struct gsva_key_v1 key;
    uint64_t uba;
    uint64_t local_pa;
    uint64_t offset;
    uint64_t page_size;
    uint32_t access_flags;
    uint32_t p_tag;
    uint32_t token_id;
    uint32_t token_value;
    bool hit;
};
```

Hook rules:

- The hook runs only after `get_phys_addr()` succeeds.
- Instruction fetch must not enter GSVA unless an explicit executable GSVA feature is added later.
- Data load maps to GSVA ReadAcquire.
- Data store maps to GSVA WriteAcquire.
- The hook must call the same `gsva_route`, token validation, and `gsva_coherence` code used by imported PA validation.
- If no GSVA route covers the VA, return `hit=false` and leave normal ARM translation unchanged.
- If a GSVA route covers the VA but validation fails, raise an ARM data fault rather than silently falling back to normal translation.
- TLB metadata must include enough information to detect stale epoch on later use.
- `GSVA_EVENT_TLB_FLUSH` must be emitted when a GSVA range is unmapped, retired, or reused.
- `SIM_GVA_TCG` log tags must be absent in `GSVA_MODE=arm_mmu` acceptance runs.

V1 side-table rule:

- ARM MMU metadata is looked up from `gsva_route` side tables.
- PTE bits are not modified in V1.

### 13.4 ARM MMU mode gating

Mode gating replaces the current `SIM_GVA_TCG` boolean probe.

Required mode helper:

```c
enum gsva_mode {
    GSVA_MODE_LEGACY_SIM_DEC = 0,
    GSVA_MODE_SIM_GVA_TCG = 1,
    GSVA_MODE_ARM_MMU = 2,
};

enum gsva_mode gsva_get_mode(void);
bool gsva_strict_enabled(void);
```

Rules:

- `GSVA_MODE=sim_gva_tcg` may continue to call `sim_dec_gva_tcg_translate()`.
- `SIM_GVA_TCG=1` is only a compatibility alias for `GSVA_MODE=sim_gva_tcg`.
- `GSVA_MODE=arm_mmu` must call `gsva_arm_mmu_translate()`.
- `GSVA_MODE=legacy_sim_dec` must not probe GSVA in `arm_cpu_tlb_fill()`.
- `GSVA_MODE=arm_mmu` must log `GSVA_TLB`, not `GVA_TCG_TRANSLATE`.

### 13.5 ARM MMU TLB metadata side table

V1 stores GSVA TLB metadata in a QEMU side table keyed by:

```text
CPUState pointer
mmu_idx
page_va
```

Minimum metadata:

```c
struct gsva_tlb_meta_v1 {
    struct gsva_key_v1 key;
    uint64_t page_va;
    uint64_t uba;
    uint64_t local_pa;
    uint64_t page_size;
    uint32_t access_flags;
    uint32_t token_id;
    uint32_t token_value;
    uint64_t install_seq;
};
```

Rules:

- Install metadata immediately before or immediately after `tlb_set_page_full()`.
- Flush metadata whenever QEMU flushes the corresponding ARM TLB range.
- `GSVA_EVENT_TLB_FLUSH` must remove matching side-table entries.
- If a side-table entry exists but the active route epoch no longer matches, reject with `GSVA_ERR_TLB_STALE`.
- Side-table metadata is an implementation detail and must not be exposed as guest ABI.

### 13.6 ARM MMU fault behavior

Failure behavior in `arm_mmu` mode:

```text
no GSVA route for VA          -> normal ARM translation result
GSVA route hit + bad token    -> ARM data abort, GSVA_ERR_TOKEN_DENIED
GSVA route hit + stale epoch  -> ARM data abort, GSVA_ERR_STALE_EPOCH
GSVA route hit + retired key  -> ARM data abort, GSVA_ERR_SEGMENT_RETIRED
GSVA route hit + coh timeout  -> ARM data abort, GSVA_ERR_COH_TIMEOUT
```

Reason:

- Once a VA is inside a GSVA route, fallback would hide real coherence or permission failures.
- Outside a GSVA route, normal guest memory behavior must remain unchanged.

## 14. Guest Linux architecture

### 14.1 UAPI

Add:

```text
guest-linux/kernel_ub/include/uapi/ub/gsva.h
```

Required commands:

```text
OBMM_CMD_GSVA_REGISTER_APERTURE
OBMM_CMD_GSVA_ALLOC_SEGMENT
OBMM_CMD_GSVA_RETIRE_SEGMENT
OBMM_CMD_GSVA_QUERY_SEGMENT
```

Do not remove existing OBMM import/export commands.

### 14.1.1 Guest kernel GSVA ioctl ABI

GSVA segment allocation must return a complete segment descriptor. `obmm_import` must not invent `segment_id`, `epoch`, `p_tag`, or token fields locally.

ABI source of truth:

```text
guest-linux/kernel_ub/include/uapi/ub/gsva.h
```

Common constants:

```c
#define OBMM_GSVA_ABI_VERSION 1
#define OBMM_GSVA_P_TAG_AUTO  0xffffffffu

#define OBMM_GSVA_ACCESS_READ   (1u << 0)
#define OBMM_GSVA_ACCESS_WRITE  (1u << 1)

#define OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY (1u << 0)
#define OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED    (1u << 1)
#define OBMM_GSVA_SEG_F_ACTIVE                  (1u << 2)
#define OBMM_GSVA_SEG_F_RETIRED                 (1u << 3)
```

Segment descriptor:

```c
struct obmm_gsva_segment_desc_v1 {
    uint32_t version;
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t epoch;
    uint32_t home_cna;
    uint32_t owner_node_id;
    uint32_t node_count;
    uint32_t cache_policy;
    uint32_t p_tag;
    uint32_t access_flags;
    uint32_t token_id;
    uint32_t token_value;
};
```

Allocation command:

```c
struct obmm_cmd_gsva_alloc_segment_v1 {
    uint32_t version;
    uint32_t flags;

    /* input */
    uint64_t size;
    uint64_t alignment;
    uint64_t requested_home_va; /* 0 means allocate from home aperture */
    uint32_t home_node_id;
    uint32_t cache_policy;
    uint32_t requested_p_tag;   /* OBMM_GSVA_P_TAG_AUTO means derive */
    uint32_t access_flags;

    /* output */
    struct obmm_gsva_segment_desc_v1 desc;
};
```

Query command:

```c
struct obmm_cmd_gsva_query_segment_v1 {
    uint32_t version;
    uint32_t flags;

    /* input: either segment_id or home_va must be non-zero */
    uint64_t segment_id;
    uint64_t home_va;

    /* output */
    struct obmm_gsva_segment_desc_v1 desc;
};
```

Retire command:

```c
struct obmm_cmd_gsva_retire_segment_v1 {
    uint32_t version;
    uint32_t flags;

    /* input */
    uint64_t segment_id;
    uint64_t epoch;
    uint32_t timeout_ms;
    uint32_t reserved;

    /* output */
    uint64_t committed_epoch;
    uint32_t status;
    uint32_t error;
};
```

Retire status:

```c
#define OBMM_GSVA_RETIRE_COMMITTED       1
#define OBMM_GSVA_RETIRE_ABORTED         2
#define OBMM_GSVA_RETIRE_PENDING_TIMEOUT 3
```

### 14.1.2 Segment allocation field rules

Allocation source:

- `gva_manager --alloc` calls `OBMM_CMD_GSVA_ALLOC_SEGMENT`.
- The kernel validates the active GSVA aperture and returns `obmm_gsva_segment_desc_v1`.
- The returned descriptor is stored with the OBMM region/import metadata.
- Importers receive the descriptor through bootstrap/import metadata and may confirm it with `OBMM_CMD_GSVA_QUERY_SEGMENT`.

Field generation rules:

```text
segment_id:
  allocated by the home manager
  V1 deterministic form: (home_cna << 48) | local_segment_counter

home_va:
  if requested_home_va != 0, must be page-aligned and inside active home aperture
  if requested_home_va == 0, allocated from the home node's GSVA aperture slice

size:
  request size rounded up to page size

epoch:
  first allocation uses 1
  explicit reuse increments epoch
  new segment_id starts again at epoch 1

p_tag:
  if requested_p_tag != OBMM_GSVA_P_TAG_AUTO, use requested_p_tag after validation
  if requested_p_tag == OBMM_GSVA_P_TAG_AUTO, V1 derives p_tag from home_cna
  deterministic V1 derivation: p_tag = home_cna & 0x00ffffffu

cache_policy:
  copied from request after validation
  DIRECTORY_MESI remains value 4

token_id:
  allocated by the home manager
  must be non-zero

token_value:
  generated by the home manager
  must be non-zero when OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED is set

access_flags:
  copied from request after validation
  must contain READ and may contain WRITE
```

### 14.1.3 `obmm_import` to `gsva_key_v1` mapping

`obmm_import` builds `gsva_key_v1` only from the segment descriptor and active mapping context.

Mapping table:

```text
gsva_key_v1.version      <- 1
gsva_key_v1.flags        <- 0 unless a documented GSVA key flag is added
gsva_key_v1.segment_id   <- desc.segment_id
gsva_key_v1.home_va      <- desc.home_va
gsva_key_v1.size         <- desc.size
gsva_key_v1.vmid         <- 0 in V1
gsva_key_v1.asid         <- 0 in V1
gsva_key_v1.pte_offset   <- 0 in strict GSVA V1
gsva_key_v1.p_tag        <- desc.p_tag
gsva_key_v1.cache_policy <- desc.cache_policy
gsva_key_v1.epoch        <- desc.epoch
```

`SIM_DEC_OP_GSVA_MAP_V1` field source:

```text
key              <- gsva_key_v1 above
local_pa         <- imported/exported local backing PA or CPU-window PA
local_va         <- mmap result, must equal desc.home_va in strict GSVA
remote_uba       <- desc.home_va in strict GSVA
token_id         <- desc.token_id
token_value      <- desc.token_value or import private token_value
source           <- GSVA_SOURCE_IMPORT_PA_WINDOW or GSVA_SOURCE_ARM_MMU
address_profile  <- GSVA_ADDRESS_PROFILE_STRICT_GSVA
```

Validation:

- `obmm_import` must reject a descriptor whose `segment_id`, `home_va`, `size`, `epoch`, `p_tag`, or `cache_policy` conflicts with an already active local import.
- `obmm_import` must reject strict GSVA if `local_va != desc.home_va`.
- QEMU must reject `SIM_DEC_OP_GSVA_MAP_V1` if the supplied fields do not match the active descriptor/route.

### 14.2 OBMM import/export path

Update:

```text
guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_export.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c
```

Required behavior:

- `MAP_GSVA` mmap must use a fixed GSVA address.
- Strict GSVA must reject `mmap(NULL, ...)`.
- Strict GSVA must reject a returned VA different from requested `home_va`.
- `obmm_import` must build `gsva_key_v1`.
- `obmm_shm_dev` must reject mapping outside registered GSVA aperture.
- unimport must emit `GSVA_EVENT_UNMAP`.
- retire must emit `GSVA_EVENT_SEGMENT_RETIRE`.
- reuse must carry the new epoch.

### 14.3 MAP_GSVA mmap design

The guest kernel already consumes `MAP_GSVA` in the generic mmap path and forwards it to OBMM through `OBMM_MMAP_FLAG_GSVA`.

Primary files:

```text
guest-linux/kernel_ub/mm/mmap.c
guest-linux/kernel_ub/include/uapi/asm-generic/mman-common.h
guest-linux/kernel_ub/include/linux/mm.h
guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c
guest-linux/aarch64/common/obmm_common.h
```

Current mechanism to preserve:

```text
userspace mmap(..., MAP_SHARED | MAP_GSVA, fd, offset)
  -> mmap_consume_gsva_flag()
  -> require file-backed shared mapping
  -> require file->f_op->mmap_supported_flags & MAP_GSVA
  -> clear MAP_GSVA before generic mmap processing
  -> encode OBMM_MMAP_FLAG_GSVA in pgoff
  -> obmm_shm_dev mmap path validates GSVA segment and fixed VA
```

User-facing rule:

```text
MAP_GSVA means "map this OBMM GSVA segment exactly at the segment home VA".
```

Required userspace call shape in strict mode:

```c
void *p = mmap((void *)home_va,
               size,
               PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_GSVA,
               shmdev_fd,
               segment_offset);
```

Strict mode rejects:

```text
mmap(NULL, ..., MAP_GSVA, ...)
MAP_GSVA | MAP_ANONYMOUS
non-shared MAP_GSVA mapping
file without mmap_supported_flags & MAP_GSVA
returned VA != requested home_va
mapping outside active GSVA aperture
mapping a non-GSVA OBMM region with MAP_GSVA
mapping a GSVA OBMM region without MAP_GSVA
mapping that overlaps active GSVA aperture without being a GSVA mapping
```

Required kernel aperture state:

```c
struct obmm_cmd_gsva_aperture {
    uint64_t base;
    uint64_t size;
    uint64_t generation;
    uint32_t node_id;
    uint32_t node_count;
    uint64_t flags;
};
```

Required aperture APIs:

```text
gsva_reserved_aperture_register(base, size, generation)
gsva_reserved_aperture_clear(generation)
gsva_reserved_aperture_overlaps(start, len)
obmm_gsva_aperture_contains(start, len)
```

`gva_manager --bootstrap` must register the aperture before any strict `MAP_GSVA` mmap.

Required mmap validation in `obmm_shm_dev`:

```text
1. detect OBMM_MMAP_FLAG_GSVA from vma->vm_pgoff
2. clear OBMM_MMAP_FLAG_GSVA before normal region offset validation
3. require region_gsva_segment(reg)
4. compute expected_start = reg->gsva_base + offset
5. require expected_start == vma->vm_start
6. require [vma->vm_start, vma->vm_end) inside active GSVA aperture
7. reject GSVA segment mmap without OBMM_MMAP_FLAG_GSVA
8. reject non-GSVA mmap overlapping active GSVA aperture
```

Required import interaction:

```text
1. exporter publishes segment metadata: home_va, size, segment_id, epoch, token_id
2. importer receives metadata and token_value
3. importer opens shmdev for the imported memory object
4. importer calls mmap(home_va, size, MAP_FIXED_NOREPLACE | MAP_GSVA)
5. kernel validates aperture and region lease
6. obmm_import builds gsva_key_v1
7. sim decoder backend sends SIM_DEC_OP_GSVA_MAP_V1
```

Required failure mapping:

```text
bad MAP_GSVA flag use        -> -EINVAL
unsupported file mapping     -> -EOPNOTSUPP
missing fd                   -> -EBADF
aperture mismatch            -> -EINVAL
address already occupied     -> -EEXIST from MAP_FIXED_NOREPLACE
QEMU feature missing         -> -EOPNOTSUPP and GSVA_ERR_FEATURE_MISSING in logs
token denied by QEMU         -> -EACCES and GSVA_ERR_TOKEN_DENIED in logs
```

CLI requirement:

```text
gsva_lifecycle_test --mode mmap_strict
gsva_lifecycle_test --mode mmap_reloc_reject
gsva_lifecycle_test --mode mmap_aperture_overlap
```

Required assertions:

```text
MAP_GSVA segment mmap succeeds exactly at home_va
mmap(NULL, MAP_GSVA) fails in strict mode
non-GSVA mmap overlapping aperture fails
GSVA segment mmap without MAP_GSVA fails
```

### 14.4 SIM decoder backend

Update:

```text
guest-linux/kernel_ub/drivers/ub/ubus/sim/
```

Required behavior:

- Preserve legacy `SIM_DEC_OP_MAP`.
- Add `SIM_DEC_OP_GSVA_*`.
- Add runtime feature query for QEMU GSVA support.
- Fail strict GSVA if QEMU does not advertise required support.
- Keep `SIM_DEC_OP_GVA_MAP` for transition mode only.

### 14.5 CLI tools

Every GSVA feature must have a CLI.

Required tools:

```text
guest-linux/aarch64/apps/gva_manager
guest-linux/aarch64/apps/gsva_lifecycle_test
guest-linux/aarch64/apps/gsva_coh_test
guest-linux/aarch64/apps/gsva_query
```

Required modes:

```text
gva_manager --bootstrap --node-id N --node-count M
gva_manager --alloc --size BYTES --cache-policy directory-mesi
gva_manager --retire --segment-id ID
gva_manager --query --segment-id ID
gsva_lifecycle_test --mode retire_reuse
gsva_lifecycle_test --mode stale_epoch
gsva_coh_test --mode write_read
gsva_coh_test --mode writer_inv
gsva_coh_test --mode retire_while_shared
gsva_query --caps
gsva_query --route --segment-id ID
gsva_query --coherence --segment-id ID
```

CLI output rule:

```text
run_id=<id>
mode=<legacy_sim_dec|sim_gva_tcg|arm_mmu>
node_count=<2|4|8>
verdict=<PASS|FAIL>
failure_reason=<stable error name>
```

## 15. GSVA coherence model

### 15.1 Object state

```c
enum gsva_coh_state {
    GSVA_COH_I = 0,
    GSVA_COH_S = 1,
    GSVA_COH_E = 2,
    GSVA_COH_M = 3,
    GSVA_COH_RETIRED = 4,
};

struct gsva_coh_object {
    struct gsva_key_v1 key;
    enum gsva_coh_state state;
    uint32_t home_cna;
    uint32_t owner_cna;
    uint64_t sharer_bitmap;
    uint64_t epoch;
    bool pending;
    uint64_t pending_seq;
};
```

V1 node limit:

- `sharer_bitmap` supports up to 64 CNAs.
- Acceptance scope is 2/4/8 nodes.
- More than 64 nodes requires replacing the bitmap with a dynamic holder set.

### 15.2 Events

```text
MapShared
MapExclusive
ReadAcquire
WriteAcquire
Invalidate
InvalidateAck
Downgrade
DowngradeAck
Writeback
WritebackAck
Fence
FenceAck
Unmap
Retire
RetireAck
Reuse
TokenChange
TLBFlush
```

### 15.3 Transition table

```text
I + ReadAcquire        -> S, add requester sharer
I + WriteAcquire       -> M, owner=requester
S + ReadAcquire        -> S, add requester sharer
S + WriteAcquire       -> pending invalidate sharers except requester
S + all InvalidateAck  -> M, owner=requester, sharers=0
E + ReadAcquire        -> S, owner becomes sharer, requester sharer
E + WriteAcquire owner -> M, owner unchanged
E + WriteAcquire other -> pending downgrade/invalidate owner
M + ReadAcquire other  -> pending writeback or data-forward, then S
M + WriteAcquire other -> pending writeback/invalidate owner, then M owner=other
any + Retire           -> pending revoke all holders
pending + all ACK      -> requested terminal state
any + stale epoch      -> reject GSVA_ERR_STALE_EPOCH
RETIRED + any access   -> reject GSVA_ERR_SEGMENT_RETIRED
```

### 15.4 Ordering and idempotency

All GSVA coherence operations are ordered by:

```text
base identity + epoch + pending_seq
```

Rules:

- A new operation cannot commit while `pending=true`.
- Retry with the same `(requester, operation, pending_seq)` is idempotent.
- Retry with a different `pending_seq` while pending returns `GSVA_ERR_COH_PENDING`.
- Retry with stale epoch returns `GSVA_ERR_STALE_EPOCH`.
- Timeout returns `GSVA_ERR_COH_TIMEOUT` and leaves the object unavailable for new maps until explicitly aborted or repaired.

### 15.5 Coherence message protocol

GSVA coherence reuses the existing UBC msgq / UB Link transport used by OBMM coherence.

Transport stack:

```text
gsva_coherence.c
  -> gsva_coh_send_ub_link_msg()
  -> obmm_coh_send_ub_link_msg() transport helper or equivalent shared helper
  -> UBC msg_code = UBC_MSG_CODE_URMA_DATA
  -> UBC sub_msg_code = UBC_MSG_SUB_GSVA_COH carrier
  -> GsvaCohMsgV1.op identifies INV/ACK/DOWNGRADE/WB/FENCE/RETIRE/TOKEN
  -> hw/ub/hisi/ubc_msgq.c receive dispatch
  -> gsva_coh_handle_rx_*()
```

Do not use SIM_DEC control opcodes for node-to-node coherence traffic.

Reason:

- `SIM_DEC_OP_GSVA_*` configures local QEMU state from the guest.
- Cross-node invalidation, downgrade, writeback, and ACK messages are data-plane coherence messages and must follow the same UB Link path as existing OBMM coherence.

### 15.6 UBC sub-message allocation

Existing UBC msg subcodes are occupied through `15`:

```c
#define UBC_MSG_SUB_COH_GETS           5
#define UBC_MSG_SUB_COH_GETM           6
#define UBC_MSG_SUB_COH_INV            7
#define UBC_MSG_SUB_COH_INV_ACK        8
#define UBC_MSG_SUB_COH_WB             9
#define UBC_MSG_SUB_COH_WB_ACK        10
#define UBC_MSG_SUB_COH_DATA          11
#define UBC_MSG_SUB_COH_FENCE         12
#define UBC_MSG_SUB_COH_FENCE_ACK     13
#define UBC_MSG_SUB_COH_DOWNGRADE     14
#define UBC_MSG_SUB_COH_DOWNGRADE_ACK 15
```

The UBC extended header stores `sub_msg_code` in 4 bits. Values above `15` are truncated on the wire and are invalid for real transport.

GSVA coherence therefore uses a single 4-bit carrier subcode and carries the concrete operation in `GsvaCohMsgV1.op`:

```c
#define UBC_MSG_SUB_GSVA_COH 15
```

`15` is also the legacy OBMM `UBC_MSG_SUB_COH_DOWNGRADE_ACK` value. Receive dispatch must first identify `sub=15` GSVA carrier payloads by `sizeof(GsvaCohMsgV1)`, `version=1`, and a valid `op`, then fall through to existing OBMM handling for non-GSVA payloads.

Add this constant in:

```text
vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h
```

Receive dispatch must be added in:

```text
vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c
```

### 15.7 GSVA coherence payload

All GSVA coherence request and ACK payloads start with:

```c
struct gsva_coh_msg_v1 {
    uint32_t version;
    uint32_t op;
    uint64_t seq;
    uint32_t source_cna;
    uint32_t target_cna;
    struct gsva_key_v1 key;
    uint64_t access_va;
    uint64_t access_len;
    uint32_t access_flags;
    uint32_t error;
};
```

Payload rules:

- `version` must equal 1.
- `op` identifies the concrete coherence operation because the wire `sub_msg_code` is the GSVA carrier.
- `seq` is allocated by the sender from `ubc_dev->next_coh_req_id` or a dedicated `next_gsva_coh_req_id`.
- `source_cna` must match the UBC transport source CNA.
- `target_cna` must match the intended receiver.
- `key` carries full `gsva_key_v1` including epoch.
- `access_va/access_len` describe the affected GSVA range.
- `access_flags` describes read/write intent.
- `error` is `GSVA_OK` for requests and stable GSVA error code for ACKs.

Message op values:

```c
#define GSVA_COH_MSG_INVALIDATE      1
#define GSVA_COH_MSG_INVALIDATE_ACK  2
#define GSVA_COH_MSG_DOWNGRADE       3
#define GSVA_COH_MSG_DOWNGRADE_ACK   4
#define GSVA_COH_MSG_WRITEBACK       5
#define GSVA_COH_MSG_WRITEBACK_ACK   6
#define GSVA_COH_MSG_FENCE           7
#define GSVA_COH_MSG_FENCE_ACK       8
#define GSVA_COH_MSG_RETIRE          9
#define GSVA_COH_MSG_RETIRE_ACK      10
#define GSVA_COH_MSG_TOKEN_REVOKE    11
#define GSVA_COH_MSG_TOKEN_ACK       12
```

Timeout:

```text
default: GSVA_COH_TIMEOUT_MS=5000
```

Rules:

- ACK must echo the original `seq`.
- ACK with mismatched `seq`, wrong `source_cna`, wrong `target_cna`, or mismatched key is ignored and logged as `GSVA_COH_ACK_MISMATCH`.
- Error ACK is terminal for that operation.
- Timeout does not silently commit.

### 15.8 Send/receive rules

Send path:

```text
1. caller holds object lock and decides required remote operations
2. caller creates pending record for each target CNA
3. caller releases object lock before blocking
4. caller sends `UBC_MSG_SUB_GSVA_COH` through UB Link with concrete `GsvaCohMsgV1.op`
5. caller polls receive links while waiting
6. ACK handler completes pending record
7. caller reacquires object lock and commits transition if all ACKs succeeded
```

Receive path:

```text
1. ubc_msgq receives msg_code=UBC_MSG_CODE_URMA_DATA
2. dispatch by `UBC_MSG_SUB_GSVA_COH` carrier and `GsvaCohMsgV1.op`
3. validate payload length and version
4. validate source CNA from transport header
5. lookup local GSVA object or tombstone
6. apply local invalidate/downgrade/writeback/retire/token-revoke action
7. flush local GSVA TLB metadata if required
8. send matching ACK with same seq and stable error code
```

Local target rule:

- If `target_cna == local_cna`, do not send over UB Link.
- Execute the receive handler locally and complete the pending record synchronously.

Retry rule:

- Sender retries only if no ACK arrives before `GSVA_COH_TIMEOUT_MS`.
- Duplicate request with same `(source_cna, seq, op, key)` is idempotent.
- Duplicate request with same `seq` but different key returns an error ACK and logs `GSVA_COH_SEQ_CONFLICT`.

Transport failure mapping:

```text
UB Link send failure       -> GSVA_ERR_COH_TIMEOUT
payload version mismatch   -> GSVA_ERR_BAD_VERSION
key mismatch at receiver   -> GSVA_ERR_KEY_MISMATCH
stale epoch at receiver    -> GSVA_ERR_STALE_EPOCH
retired object at receiver -> GSVA_ERR_SEGMENT_RETIRED
```

### 15.9 Locking rule

QEMU implementation must use:

- A global route/coherence table lock only for object lookup, insert, and remove.
- A per-object lock for state transitions.
- No blocking remote message wait while holding the global table lock.
- No PA-MESI fence/writeback while holding the global table lock.

Reason:

- Retire/reuse and writer invalidation would otherwise deadlock under multi-node tests.

### 15.10 PA-MESI relationship

For a write:

```text
GSVA WriteAcquire
  -> owner permission granted
  -> obmm_coh_cpu_write / obmm_coh_dma_write
```

For a read:

```text
GSVA ReadAcquire
  -> shared permission granted
  -> obmm_coh_cpu_read / obmm_coh_dma_read
```

For retire:

```text
GSVA Retire
  -> revoke GSVA holders
  -> PA-MESI fence/writeback
  -> unmap route
  -> mark RETIRED
```

## 16. Segment lifecycle transaction

### 16.1 Coordinator

The segment home manager is the coordinator.

Coordinator identity:

```text
home_cna from export/bootstrap metadata
```

### 16.2 Retire sequence

```text
1. coordinator emits GSVA_EVENT_SEGMENT_RETIRE(key, epoch)
2. QEMU marks key pending retire
3. QEMU sends revoke/invalidate to all holders
4. holders drop local GSVA state and flush TLB for range
5. holders ACK retire
6. coordinator issues PA-MESI fence/writeback
7. route entry is removed but a retired tombstone is retained
8. key enters GSVA_COH_RETIRED
9. guest manager receives retire committed
```

Retired tombstone rule:

- Keep retired base identity and last epoch until end of run or explicit cleanup.
- Tombstone lookup returns `GSVA_ERR_SEGMENT_RETIRED` or `GSVA_ERR_STALE_EPOCH`.
- Tombstone prevents stale requests from appearing as generic route misses.

### 16.3 Reuse sequence

```text
1. old key must be RETIRED
2. new mapping uses same segment_id with higher epoch, or a new segment_id with epoch 1
3. route is installed with the new active generation
4. old epoch requests are rejected
5. new map requests must carry the new epoch
```

Preferred V1 rule:

- New allocation uses a new `segment_id`.
- Explicit reuse test may reuse `segment_id` with higher `epoch`.

### 16.4 Timeout behavior

Allowed terminal outcomes:

```text
RETIRE_COMMITTED
RETIRE_ABORTED
RETIRE_PENDING_TIMEOUT
```

If timeout occurs:

- New maps for the same base identity are rejected.
- Existing stale holders are treated as invalid.
- Query reports `GSVA_ERR_COH_TIMEOUT`.
- Manual cleanup may abort or force-retire only through a diagnostic command.

## 17. Stats and diagnostics

### 17.1 GSVA stats

```text
gsva_map_total
gsva_unmap_total
gsva_key_mismatch_total
gsva_stale_epoch_total
gsva_retire_total
gsva_retire_timeout_total
gsva_tlb_flush_total
gsva_coh_gets_total
gsva_coh_getm_total
gsva_coh_inv_total
gsva_coh_wb_total
```

### 17.2 Route stats

```text
gva_ma_lookup_total
gva_ma_miss_total
gva_mp_lookup_total
gva_mp_miss_total
```

### 17.3 PA-MESI stats

PA-MESI stats remain separate:

```text
obmm_coh_gets_total
obmm_coh_getm_total
obmm_coh_inv_total
obmm_coh_wb_total
obmm_coh_fence_total
```

### 17.4 Required log tags

```text
GSVA_MAP
GSVA_UNMAP
GSVA_KEY
GSVA_COH
GSVA_RETIRE
GSVA_ROUTE
GSVA_TLB
```

Acceptance output must include:

```text
run_id=<id>
mode=<legacy_sim_dec|sim_gva_tcg|arm_mmu>
node_count=<2|4|8>
verdict=<PASS|FAIL>
failure_reason=<stable error name>
```

## 18. Architecture risks

Risk: ARM MMU hook scope is broader than the existing SIM_DEC path.

Mitigation:

- V1 uses route side tables rather than PTE bit encoding.
- `sim_gva_tcg` remains available as a transition mode.

Risk: GSVA coherence and PA-MESI both appear to own coherence.

Mitigation:

- GSVA decides semantic validity and ownership.
- PA-MESI only moves and orders data after GSVA grants permission.

Risk: retire/reuse races leave stale state visible.

Mitigation:

- Retire uses pending state, ACK, fence/writeback, route removal, and tombstone.
- Reuse requires higher epoch or new segment ID.

Risk: debugging multi-node failures is hard.

Mitigation:

- Every acceptance run prints run ID, mode, node count, verdict, and stable failure reason.
- Stats are separated by GSVA, route, and PA-MESI layers.

## Existing Code Integration Map

This section maps the plan's requirements to existing code, identifying what exists and what must be added.

### QEMU opcode dispatch

Location: `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`

The dispatch is a switch statement in `ubc_handle_sim_dec_message()`. Each opcode has a handler function following the pattern:

```c
static int sim_dec_handle_map(const SimDecMapReq *req, SimDecMapResp *resp)
static int sim_dec_handle_unmap(const SimDecUnmapReq *req)
static int sim_dec_handle_gva_map(const SimDecGvaMapReq *req, SimDecMapResp *resp)
```

Payload access: `data + sizeof(*hdr)` for request, `memcpy(resp + sizeof(*resp_hdr), &payload, sizeof(payload))` for response. Each handler validates minimum length before processing.

New GSVA opcodes (0x09-0x0c) add cases to this switch and follow the same handler signature pattern.

The existing `SIM_DEC_OP_GVA_MAP` (0x07) handler already validates GSVA identity profile:

- `SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY = 2`
- Requires `local_va == home_va == remote_uba` and `pte_offset = 0`

The existing `SimDecGvaMapReq` struct carries: vmid, asid, home_va, pte_offset, p_tag, cache_policy, address_profile, gva_id. GSVA V1 opcodes extend this with: version, segment_id, epoch, and versioned ABI validation.

### OBMM coherence (PA-MESI) data layer

Location: `vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.c/h`

Public functions that GSVA coherence calls after permission is granted:

```c
MemTxResult obmm_coh_read(BusControllerDev *ubc_dev, uint64_t remote_uba,
                          uint32_t token_id, uint32_t dcna, uint8_t *buf, uint32_t len);
MemTxResult obmm_coh_write(BusControllerDev *ubc_dev, uint64_t remote_uba,
                           uint32_t token_id, uint32_t dcna, const uint8_t *buf, uint32_t len);
int obmm_coh_send_fence(BusControllerDev *ubc_dev, uint32_t home_cna,
                        uint64_t range_start, uint64_t range_len, uint32_t token_id);
void obmm_coh_invalidate_local_range(BusControllerDev *ubc_dev, uint32_t home_cna,
                                     uint64_t range_start, uint64_t range_len, uint32_t token_id);
MemTxResult obmm_coh_local_read(BusControllerDev *ubc_dev, uint64_t uba,
                                uint32_t token_id, uint8_t *buf, uint32_t len);
MemTxResult obmm_coh_local_write(BusControllerDev *ubc_dev, uint64_t uba,
                                 uint32_t token_id, const uint8_t *buf, uint32_t len);
```

Inter-node transport:

```c
int obmm_coh_send_ub_link_msg(BusControllerDev *ubc_dev, uint32_t dcna,
                              uint8_t sub_msg_code, const void *payload, uint32_t payload_len);
void obmm_coh_poll_rx_links(BusControllerDev *ubc_dev);
```

Per-line state structures (informational -- GSVA coherence state is per-object, not per-line):

```c
typedef struct ObmmCohLocalLine {
    BusControllerDev *ubc_dev;
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
    ObmmCoherenceState state;  // OBMM_COH_I/S/E/M
    bool dirty;
    uint8_t data[OBMM_COH_LINE_SIZE];  // 64 bytes
} ObmmCohLocalLine;

typedef struct ObmmCohDirLine {
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t token_id;
    uint32_t owner_cna;
    uint32_t sharer_count;
    uint32_t sharers[OBMM_COH_MAX_SHARERS];  // 64 max
    ObmmCoherenceState state;
    uint64_t version;
    bool has_owner;
    bool dirty;
    bool pending;
    bool data_valid;
    uint8_t data[OBMM_COH_LINE_SIZE];
} ObmmCohDirLine;
```

Integration rule:

- GSVA coherence grants semantic permission.
- Then calls `obmm_coh_read()`/`obmm_coh_write()` to move bytes.
- GSVA coherence uses `obmm_coh_send_ub_link_msg()` for inter-node messages, same transport as existing PA-MESI.
- GSVA coherence state is per-GSVA-object; PA-MESI state remains per-line.

### ARM MMU hook point

Location: `vendor/qemu_8.2.0_ub/target/arm/tcg/tlb_helper.c`

The hook is in `arm_cpu_tlb_fill()`:

```c
bool arm_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool probe, uintptr_t retaddr)
```

Current SIM_GVA_TCG integration:

```c
if (access_type != MMU_INST_FETCH &&
    sim_dec_gva_tcg_translate(address, access_type == MMU_DATA_STORE,
                              &gva_local_pa, &gva_page_size)) {
    res.f.phys_addr = gva_local_pa & TARGET_PAGE_MASK;
    res.f.lg_page_size = ctz64(gva_page_size);
}
```

Existing hook functions:

```c
bool sim_dec_gva_tcg_enabled(void);  // checks SIM_GVA_TCG env var
bool sim_dec_gva_tcg_translate(uint64_t va, bool is_write,
                               uint64_t *local_pa, uint64_t *page_size);
```

ARM MMU V1 approach:

- In `arm_cpu_tlb_fill()`, after `get_phys_addr()`, add a mode check for `arm_mmu`.
- When `GSVA_MODE=arm_mmu`, call `gsva_arm_mmu_translate()` instead of `sim_dec_gva_tcg_translate()`.
- `gsva_arm_mmu_translate()` uses `gsva_route` for lookup and `gsva_coherence` for permission.
- TLB flush via existing `sim_dec_flush_gva_tlbs()` which calls `tlb_flush_all_cpus_synced()`.
- Hook only applies to data accesses (`access_type != MMU_INST_FETCH`).

### Guest kernel SIM decoder

Location: `guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder.h`

Existing opcode enum:

```c
enum sim_dec_opcode {
    SIM_DEC_OP_MAP                    = 0x01,
    SIM_DEC_OP_UNMAP                  = 0x02,
    SIM_DEC_OP_SYNC                   = 0x03,
    SIM_DEC_OP_QUERY                  = 0x04,
    SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH = 0x05,
    SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP  = 0x06,
    SIM_DEC_OP_GVA_MAP                = 0x07,
    SIM_DEC_OP_COH_FENCE              = 0x08,
};
```

New GSVA opcodes add to this enum: 0x09-0x0c.

Existing MAP request struct:

```c
struct sim_dec_map_req {
    u64 local_pa;
    u64 size;
    u64 remote_uba;
    u32 token_id;
    u32 token_value;
    u32 scna;
    u32 dcna;
    u8  seid[16];
    u8  deid[16];
    u32 upi;
    u32 src_eid;
};
```

### Guest OBMM import path

Location: `guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c`

MAP_GSVA handling already exists:

- Validates `OBMM_MMAP_FLAG_GSVA` flag
- Checks GSVA aperture containment
- Requires identity mapping constraints for GSVA segments

Import priv struct (version 2, existing):

```c
struct obmm_sim_dec_import_priv_v2 {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t remote_uba;
    uint32_t token_value;
    uint32_t map_source;
    uint32_t address_profile;
    uint32_t cache_policy;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint32_t vmid;
    uint32_t asid;
    uint32_t tid;
    uint32_t p_tag;
    uint32_t access_flags;
    uint64_t gva_id;
};
```

This struct already carries vmid, asid, home_va, pte_offset, p_tag, cache_policy, address_profile. Missing: segment_id, epoch. These will be added as part of the GSVA V1 extension.

### Guest OBMM UAPI

Location: `guest-linux/kernel_ub/include/uapi/ub/obmm.h`

Existing GSVA aperture commands:

```c
#define OBMM_CMD_GSVA_APERTURE_REGISTER _IOW('x', 10, struct obmm_cmd_gsva_aperture)
#define OBMM_CMD_GSVA_APERTURE_QUERY    _IOWR('x', 11, struct obmm_cmd_gsva_aperture)
#define OBMM_CMD_GSVA_APERTURE_CLEAR    _IOW('x', 12, struct obmm_cmd_gsva_aperture)
```

Existing GSVA mmap flag:

```c
#define OBMM_MMAP_FLAG_GSVA  (1UL << 62)
```

New GSVA segment ioctls defined in Section 14.1.1 will use the `'x'` ioctl magic with command numbers after the existing aperture commands.

### Guest MAP_GSVA kernel path

Location: `guest-linux/kernel_ub/mm/mmap.c`

The guest kernel already consumes `MAP_GSVA` in the generic mmap path:

```text
userspace mmap(..., MAP_SHARED | MAP_GSVA, fd, offset)
  -> mmap_consume_gsva_flag()
  -> require file-backed shared mapping
  -> require file->f_op->mmap_supported_flags & MAP_GSVA
  -> clear MAP_GSVA before generic mmap processing
  -> encode OBMM_MMAP_FLAG_GSVA in pgoff
  -> obmm_shm_dev mmap path validates GSVA segment and fixed VA
```

This existing mechanism is preserved and extended with strict validation rules defined in Section 14.3.

### GVA Manager app

Location: `guest-linux/aarch64/apps/gva_manager/gva_manager.c`

Current capabilities:

- GSVA aperture agreement protocol
- Segment allocation/retirement coordination
- OBMM GSVA aperture registration via ioctl

New `OBMM_CMD_GSVA_ALLOC_SEGMENT` and `OBMM_CMD_GSVA_RETIRE_SEGMENT` ioctls defined in Section 14.1.1 extend `gva_manager --alloc` and `gva_manager --retire`.

### QEMU build integration

Location: `vendor/qemu_8.2.0_ub/hw/ub/meson.build`

All `.c` files are compiled into a single source set (`ub_ss`), conditional on `CONFIG_HW_UB`. New GSVA modules (`gsva_key.c`, `gsva_route.c`, `gsva_coherence.c`, `gsva_stats.c`) are added to this source set.

## Part II: Implementation Plan

## 19. Development rules

Implementation must follow these rules:

- Add CLI coverage for every new GSVA feature.
- Add tests for every new GSVA feature.
- Preserve legacy `SIM_DEC_OP_MAP` layout.
- Do not use PA as GSVA coherence identity.
- Do not treat token as key identity.
- Do not allow strict GSVA to relocate mmap address.
- Do not commit segment reuse before retire ACK and fence completion.
- Do not merge GSVA stats into OBMM PA-MESI stats.
- Do not accept cache policy mutation in place.

## 20. Milestone 0: Protocol freeze

Purpose:

- Make ABI and behavior deterministic before implementation.

Deliverables:

- This document is the canonical protocol source.
- `SIM_DEC_OP_GSVA_*` numeric values are fixed at `0x09` through `0x0c`.
- UAPI header path is fixed at `guest-linux/kernel_ub/include/uapi/ub/gsva.h`.
- V1 `vmid/asid` are fixed at 0.
- V1 `segment_id` allocation rule is fixed.
- V1 ARM MMU metadata uses side table, not PTE bits.

Acceptance:

```bash
true
```

Reason:

- Milestone 0 is a documentation/protocol gate. Build validation starts in Milestone 1.

## 21. Milestone 1: ABI scaffolding and dry-run query

Purpose:

- Add compile-visible protocol objects without changing default behavior.

Implementation steps:

1. Add `guest-linux/kernel_ub/include/uapi/ub/gsva.h`.
2. Add QEMU `gsva_key.c/h`.
3. Add opcode constants to guest SIM decoder header.
4. Add opcode constants to QEMU `ub_ubc.c`.
5. Add `SIM_DEC_OP_GSVA_QUERY_V1` capability query.
6. Add `GSVA_KEY` logs for query path.
7. Add `gsva_query --caps`.

Files:

```text
guest-linux/kernel_ub/include/uapi/ub/gsva.h
guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder.h
guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder_backend.c
guest-linux/aarch64/common/obmm_common.h
guest-linux/aarch64/apps/gsva_query/
vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.h
vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h
vendor/qemu_8.2.0_ub/hw/ub/meson.build
```

Acceptance:

```bash
./guest-linux/aarch64/scripts/build_guest_artifacts.sh
./guest-linux/aarch64/scripts/build_qemu_binary.sh
```

Required evidence:

```text
GSVA_KEY
GSVA_QUERY_CAPS
verdict=PASS
```

## 22. Milestone 2: GSVA map/unmap route

Purpose:

- Make strict GSVA mapping visible in QEMU route state.

Implementation steps:

1. Add `gsva_route.c/h`.
2. Implement `SIM_DEC_OP_GSVA_MAP_V1`.
3. Implement `SIM_DEC_OP_GSVA_UNMAP_V1`.
4. Validate `user_va == uba == home_va` in strict mode.
5. Reject `mmap(NULL, ...)` in strict mode.
6. Reject mappings outside registered GSVA aperture.
7. Keep retired tombstone on unmap/retire paths where needed.
8. Add `GSVA_MAP`, `GSVA_UNMAP`, and `GSVA_ROUTE` logs.
9. Add `run_ub_two_node_gsva_identity_test.sh`.
10. Add `run_ub_four_node_gsva_identity_test.sh`.
11. Add `run_ub_eight_node_gsva_identity_test.sh`.
12. Add strict `MAP_GSVA` mmap negative tests.

Files:

```text
guest-linux/kernel_ub/mm/mmap.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_export.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c
guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder_backend.c
guest-linux/aarch64/apps/gva_manager/
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.h
vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
```

Acceptance:

```bash
GSVA_MODE=sim_gva_tcg GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_identity_test.sh
GSVA_MODE=sim_gva_tcg GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
GSVA_MODE=sim_gva_tcg GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_identity_test.sh
GSVA_TEST_MODE=mmap_strict ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_TEST_MODE=mmap_reloc_reject ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_TEST_MODE=mmap_aperture_overlap ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
```

Required assertions:

```text
user_va == uba == home_va
GSVA_MAP appears in QEMU logs
GSVA_ROUTE lookup succeeds
no relocated mmap fallback appears
MAP_GSVA invalid forms fail with expected errno
verdict=PASS
```

## 23. Milestone 3: GSVA coherence over existing PA-MESI

Purpose:

- Add GSVA semantic coherence before the existing PA-MESI data layer.

Implementation steps:

1. Add `gsva_coherence.c/h`.
2. Add `gsva_stats.c/h`.
3. Add `UBC_MSG_SUB_GSVA_COH_*` constants.
4. Add `ubc_msgq.c` receive dispatch for GSVA coherence subcodes.
5. Implement GSVA route token lease state.
6. Implement `gsva_route_validate_token`.
7. Implement GSVA object state.
8. Implement ReadAcquire with token validation before state change.
9. Implement WriteAcquire with token validation before state change.
10. Implement Invalidate and InvalidateAck.
11. Implement pending sequence and idempotent retry.
12. Implement timeout with `GSVA_COH_TIMEOUT_MS`.
13. Call PA-MESI only after GSVA token and coherence permission succeeds.
14. Add token valid/denied/rotate tests.
15. Add `gsva_coh_test`.
16. Add 2/4/8-node coherence scripts.

Files:

```text
guest-linux/aarch64/apps/gsva_coh_test/
guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
guest-linux/aarch64/scripts/run_ub_eight_node_gsva_coh_test.sh
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.h
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c
vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h
```

Acceptance:

```bash
GSVA_TEST_MODE=write_read ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=token_denied ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=token_rotate ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=write_read RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_coh_test.sh
```

Required assertions:

```text
GSVA_COH_GETS present
GSVA_COH_GETM present for writer tests
GSVA_COH_INV present for conflicting writer tests
wrong token fails with GSVA_ERR_TOKEN_DENIED
token rotation does not change gsva_key identity
OBMM_COH_* present only as data-layer evidence
verdict=PASS
```

## 24. Milestone 4: Segment lifecycle transaction

Purpose:

- Make retire/reuse atomic and diagnosable.

Implementation steps:

1. Implement `GSVA_EVENT_SEGMENT_RETIRE`.
2. Implement `GSVA_EVENT_SEGMENT_REUSE`.
3. Implement Retire and RetireAck.
4. Implement tombstone retention.
5. Implement stale epoch rejection.
6. Implement PA-MESI fence/writeback before route removal commit.
7. Implement timeout terminal states.
8. Add `gsva_lifecycle_test`.
9. Add retire-while-shared coherence case.

Files:

```text
guest-linux/aarch64/apps/gsva_lifecycle_test/
guest-linux/aarch64/scripts/run_ub_two_node_gsva_lifecycle_test.sh
guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
guest-linux/aarch64/scripts/run_ub_eight_node_gsva_lifecycle_test.sh
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
```

Acceptance:

```bash
GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_TEST_MODE=stale_epoch ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_TEST_MODE=retire_while_shared ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
```

Required assertions:

```text
GSVA_RETIRE committed
old epoch request rejected with GSVA_ERR_STALE_EPOCH
retired object access rejected with GSVA_ERR_SEGMENT_RETIRED
new epoch map succeeds only after retire commit
timeout reports GSVA_ERR_COH_TIMEOUT
verdict=PASS
```

## 25. Milestone 5: ARM MMU mode

Purpose:

- Make ARM MMU the real GSVA access path.

Implementation steps:

1. Add `gsva_mmu_lookup`.
2. Add `gsva_arm_mmu_translate`.
3. Connect `target/arm/tcg/tlb_helper.c::arm_cpu_tlb_fill()` to `gsva_arm_mmu_translate`.
4. Store GSVA key metadata in the TLB side table.
5. Emit `GSVA_TLB` logs on lookup and flush.
6. Reject stale TLB metadata with `GSVA_ERR_TLB_STALE`.
7. Ensure data access reaches route/coherence through ARM MMU metadata.
8. Keep `SIM_GVA_TCG` available only when `GSVA_MODE=sim_gva_tcg`.
9. Add ARM MMU acceptance scripts for 2/4/8 nodes.

Files:

```text
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c
vendor/qemu_8.2.0_ub/target/arm/tcg/tlb_helper.c
guest-linux/aarch64/scripts/run_ub_two_node_gsva_arm_mmu_acceptance.sh
guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
guest-linux/aarch64/scripts/run_ub_eight_node_gsva_arm_mmu_acceptance.sh
```

Acceptance:

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_arm_mmu_acceptance.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_arm_mmu_acceptance.sh
```

Required assertions:

```text
GSVA_TLB lookup present
GSVA_ROUTE lookup present
GSVA_COH present
SIM_GVA_TCG data-path hit absent
legacy SIM_DEC map may appear only for bootstrap/fallback diagnostics
verdict=PASS
```

## 26. Milestone 6: Default enablement and regression closure

Purpose:

- Make `arm_mmu` strict GSVA the default.

Implementation steps:

1. Change default `GSVA_MODE` to `arm_mmu`.
2. Change default `GSVA_STRICT` to 1 for GSVA tests.
3. Keep explicit `GSVA_MODE=legacy_sim_dec` and `GSVA_MODE=sim_gva_tcg`.
4. Run full 2/4/8-node matrix.
5. Update docs and scripts to point to this canonical document.

Acceptance:

```bash
./guest-linux/aarch64/scripts/build_guest_artifacts.sh
./guest-linux/aarch64/scripts/build_qemu_binary.sh
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_four_node_obmm_coh_test.sh
COH_TEST_MODE=all RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_obmm_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_arm_mmu_acceptance.sh
```

Required assertions:

```text
all commands print verdict=PASS
no command prints failure_reason other than GSVA_OK
GSVA_TLB present in arm_mmu runs
SIM_GVA_TCG data-path hit absent in arm_mmu runs
legacy SIM_DEC tests remain compatible
```

## 27. Validation matrix

### 27.1 Build

```bash
./guest-linux/aarch64/scripts/build_guest_artifacts.sh
./guest-linux/aarch64/scripts/build_qemu_binary.sh
```

### 27.2 Existing OBMM coherence baseline

```bash
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_four_node_obmm_coh_test.sh
COH_TEST_MODE=all RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_obmm_coh_test.sh
```

### 27.3 GSVA identity

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_identity_test.sh
```

Assertions:

```text
user_va == uba == home_va
GSVA_MAP appears in QEMU logs
no relocated mmap fallback appears
```

### 27.4 GSVA lifecycle

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=stale_epoch ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
```

Assertions:

```text
GSVA_RETIRE committed
old epoch request rejected with GSVA_ERR_STALE_EPOCH
new epoch map succeeds
```

### 27.5 GSVA coherence

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=write_read ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=retire_while_shared ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=write_read RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_coh_test.sh
```

Assertions:

```text
GSVA_COH_GETS present
GSVA_COH_GETM present for writer tests
GSVA_COH_INV present for conflicting writer tests
OBMM_COH_* present only as data-layer evidence
SIM_GVA_TCG absent when GSVA_MODE=arm_mmu
```

### 27.6 ARM MMU default path

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
```

Assertions:

```text
GSVA_TLB lookup present
GSVA_ROUTE lookup present
SIM_GVA_TCG data-path hit absent
legacy SIM_DEC map may appear only for bootstrap/fallback diagnostics
```

## 28. Definition of Done

The full project is done only when all conditions below are true:

- Default startup uses ARM MMU path for GSVA.
- `SIM_GVA_TCG` is not used in the default data path.
- `user_va == uba == home_va` holds in strict GSVA.
- GSVA identity uses base identity plus epoch, not PA.
- Token validation is separate from identity.
- Cache policy mutation requires old-key revoke/unmap and new-key map.
- Shared writes satisfy GSVA coherence: conflict detection, write-before-invalidate, no visible dirty conflict.
- Retire/reuse rejects stale epoch requests.
- Retire/reuse does not silently reuse old mappings.
- 2/4/8-node identity, coherence, lifecycle, and ARM MMU tests pass.
- Logs include run ID, mode, node count, verdict, and stable failure reason.
- Docs and scripts point to this canonical plan.

## 28.1 Current implementation status

Status as of 2026-06-09:

- ARM MMU GSVA access path is implemented and has 2/4/8-node acceptance evidence with `GSVA_TLB: lookup` and no `GVA_TCG_TRANSLATE` data-path fallback.
- GSVA segment descriptor ABI, descriptor-driven import, manager peer descriptor distribution, manager-distributed descriptor import cleanup + retire, and manager RetireAck-before-cleanup are implemented and validated.
- Token v1 ReadAcquire/WriteAcquire validation, ACK-gated token rotation, manager-distributed token revoke + holder ACK, and ARM MMU token revoke TLB flush are implemented and validated.
- Route-local GSVA coherence covers writer invalidation, retire tombstone, stale epoch rejection, higher epoch reuse, pending timeout, timeout query, timeout TLB flush, and 2/4/8-node InvAck recovery.
- Manager-distributed GSVA coherence recovery is validated in a two-node ARM MMU run.
- QEMU active UB Link GSVA remote invalidate/ACK is implemented and validated in a two-node ARM MMU run. The wire protocol uses the 4-bit-safe `UBC_MSG_SUB_GSVA_COH` carrier and `GsvaCohMsgV1.op` for concrete operations.
- QEMU active UB Link GSVA remote writeback/ACK is implemented and validated in a two-node ARM MMU run.

Latest manager-distributed recovery evidence:

```text
run_id=guest-linux/aarch64/logs/2026-06-09_03-24-04_gsva_mgr_13234
command=GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_COH_HOLD_PENDING=1 GSVA_COH_TIMEOUT_MS=10000 GVA_MANAGER_COH_RECOVERY=1 GVA_MANAGER_CACHE_POLICY=directory-mesi ./guest-linux/aarch64/scripts/run_ub_dual_node_gsva_manager_bootstrap.sh
nodeA_guest.log: manager coherence recovery committed segment_id=0xc4c2000000000001 acked_peers=1
nodeB_guest.log: manager coherence recovery pending segment_id=0xc4c2000000000001 state=1 seq=0x2 waiting_for=0x8
nodeB_guest.log: manager coherence recovery holder ack segment_id=0xc4c2000000000001 state=3 seq=0x2 cna=50386 holder_cna=3
```

Latest active UB Link remote invalidate evidence:

```text
run_id=guest-linux/aarch64/logs/2026-06-09_03-44-01_gsva_coh_27961
command=GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_COH_HOLD_PENDING=1 GSVA_COH_UB_LINK_TX=1 GSVA_COH_TIMEOUT_MS=10000 GSVA_TEST_MODE=coh_remote_inv ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
nodeA_guest.log: coh_remote_inv Query pending seq=0x2 peer_cna=50386
nodeA_guest.log: coh_remote_inv Retry error=0
nodeA_qemu.log: GSVA_COH: ub_link send sub=15 scna=0xc4c2 dcna=0xc4d2 payload_len=120
nodeB_qemu.log: GSVA_COH: ub_link rx sub=15 op=1 scna=0xc4c2 payload_len=120
nodeB_qemu.log: GSVA_COH: rx INV from cna=50370 segment_id=0x1 seq=2
nodeA_qemu.log: GSVA_COH: ub_link rx sub=15 op=2 scna=0xc4d2 payload_len=120
nodeA_qemu.log: GSVA_COH: rx INV_ACK applied from cna=50386 segment_id=0x1 seq=2 rc=0
```

Latest active UB Link remote writeback evidence:

```text
run_id=guest-linux/aarch64/logs/2026-06-09_03-53-21_gsva_coh_29956
command=GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_COH_HOLD_PENDING=1 GSVA_COH_UB_LINK_TX=1 GSVA_COH_TIMEOUT_MS=10000 GSVA_TEST_MODE=coh_remote_wb ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
nodeA_guest.log: coh_remote_wb Query pending seq=0x2 peer_cna=50386
nodeA_guest.log: coh_remote_wb Retry error=0
nodeA_qemu.log: GSVA_COH: tx WRITEBACK target=50386 seq=2 segment_id=0x1 rc=0
nodeB_qemu.log: GSVA_COH: ub_link rx sub=15 op=5 scna=0xc4c2 payload_len=120
nodeB_qemu.log: GSVA_COH: rx WRITEBACK from cna=50370 segment_id=0x1 seq=2
nodeA_qemu.log: GSVA_COH: ub_link rx sub=15 op=6 scna=0xc4d2 payload_len=120
nodeA_qemu.log: GSVA_COH: WbAck recovery grant M cna=50370 seq=2 segment_id=0x1
nodeA_qemu.log: GSVA_COH: rx WRITEBACK_ACK applied from cna=50386 segment_id=0x1 seq=2 rc=0
```

Remaining gap before this plan can be considered complete:

- QEMU GSVA coherence still needs active UB Link data-plane transactions for downgrade, fence, retire, and token revoke. Remote invalidate/ACK and writeback/ACK are now active over UB Link; the remaining operations still need full sender/receiver state transitions and validation.
- Four-node and eight-node manager-distributed GSVA recovery are not yet validated.
- Milestone 6 full default-mode regression matrix is not yet complete.

## 29. Future work

Future work must not block V1:

- Non-zero `vmid` for multi-VM simulation.
- Non-zero `asid` for per-process GSVA isolation.
- PTE-bit encoding for GSVA metadata.
- Dynamic holder set for more than 64 CNAs.
- Performance fast paths for read-mostly shared mappings.
- More realistic ARM cache microarchitecture modeling.
