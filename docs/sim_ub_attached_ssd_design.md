# UB-Attached Semantic SSD Simulation Design

## Goal

This document defines the first simulator design for a UB-attached SSD endpoint.
The goal is to prove that storage can join the UB, GSVA, and OBMM protocol stack
as a first-class endpoint while aligning with Lingqu Block and Lingqu DFS.

The V1 SSD must demonstrate:

- the SSD has its own UB identity, route, and CNA;
- guest software can submit storage commands through a device queue;
- SSD read/write commands move data through GSVA segment descriptors;
- all buffer accesses obey GSVA token, epoch, retire, fence, and coherence
  rules;
- durable bytes are represented through Lingqu Block records and named through
  Lingqu DFS manifests, not through a private SSD-only durability model.

## Non-Goals

- Do not implement full NVMe, PCIe, namespaces, admin queues, or identify data
  in V1.
- Do not implement a production filesystem or POSIX namespace.
- Do not bypass GSVA/OBMM by copying through host pointers without semantic
  validation.
- Do not let the SSD invent path names, manifests, or versioning outside Lingqu
  DFS and Lingqu Block.
- Do not model flash translation layer internals, wear leveling, or garbage
  collection in V1.

## Architecture

The SSD is a UB endpoint with a semantic block interface.

```text
guest process
  -> /dev/ub_ssd0 ioctl
  -> guest kernel ub_ssd driver
  -> QEMU ub-ssd SysBus/MMIO device
  -> local command worker
  -> GSVA route/coherence for command buffers
  -> UB Link for GSVA coherence messages when remote holders are involved
  -> Lingqu Block-compatible durable payload backend
  -> Lingqu DFS-compatible manifest namespace
```

The device interface is intentionally NVMe-like but not NVMe-compatible. It
keeps queue semantics and completion status, but the addressing model is
Lingqu Block-oriented instead of raw LBA-only.

V1 operation set:

```text
SSD_OP_BLOCK_WRITE
SSD_OP_BLOCK_READ
SSD_OP_BLOCK_SEAL
SSD_OP_BLOCK_TOMBSTONE
SSD_OP_FLUSH
SSD_OP_STAT
SSD_OP_EXPORT_SNAPSHOT
SSD_OP_IMPORT_SNAPSHOT
```

Raw LBA mode may be added later as a compatibility profile. It should not be
the first design center because Lingqu Block already defines payload identity,
versioning, checksum, seal, and tombstone semantics.

## V1 Implementation Decisions

These decisions are fixed for the first code path.

### QEMU device type

`ub-ssd` is a QEMU `SysBusDevice`, not a PCI device.

Reason:

- V1 focuses on UB, GSVA, OBMM, and Lingqu Block/DFS semantics;
- full NVMe/PCIe enumeration would add a large unrelated implementation surface;
- SysBus MMIO matches the platform-device style already used by the current UB
  simulator.

The `virt` machine instantiates one `ub-ssd` device per QEMU node during board
initialization. V1 does not expose `ub-ssd` as a user-created `-device`, because
dynamic SysBus devices are created after the board FDT is built and would not be
discoverable by the guest platform driver without a second dynamic-FDT path.

The machine-created device is configured with QOM properties equivalent to:

```text
node-id=0,cna=0xc4c22000,backend=memory,ubc=/machine/peripheral/ubcdev0
```

`UB_SIM_SKIP_DEVICES=ssd` suppresses SSD instantiation for negative discovery
tests. The `ubc` property links the SSD to the local `BusControllerDev`. The SSD
calls GSVA route/coherence and OBMM data helpers through that UBC state. It does
not own a separate route table.

### QEMU GSVA access API

The SSD must not include or dereference the GSVA static globals in `ub_ubc.c`.
In particular, it must not reach into `g_gsva_routes` or `g_gsva_coh` directly.

V1 uses exported UBC wrapper functions, not raw table getters:

```c
int ubc_gsva_device_read_acquire(BusControllerDev *ubc,
                                 const struct gsva_key_v1 *key,
                                 uint32_t requester_cna,
                                 uint64_t access_va,
                                 uint64_t access_len,
                                 uint32_t access_flags,
                                 uint64_t *pending_seq);

int ubc_gsva_device_write_acquire(BusControllerDev *ubc,
                                  const struct gsva_key_v1 *key,
                                  uint32_t requester_cna,
                                  uint64_t access_va,
                                  uint64_t access_len,
                                  uint32_t access_flags,
                                  uint64_t *pending_seq);

int ubc_gsva_device_read(BusControllerDev *ubc,
                         const struct gsva_key_v1 *key,
                         uint32_t requester_cna,
                         uint64_t gsva,
                         void *dst,
                         uint64_t len);

int ubc_gsva_device_write(BusControllerDev *ubc,
                          const struct gsva_key_v1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva,
                          const void *src,
                          uint64_t len);

int ubc_gsva_device_fence(BusControllerDev *ubc,
                          const struct gsva_key_v1 *key,
                          uint32_t requester_cna,
                          uint64_t gsva,
                          uint64_t len);
```

Reason:

- `ub_ubc.c` owns the route and coherence tables.
- Device models should depend on a stable device-facing API, not on table
  layout.
- The wrapper can preserve lock ordering and pending-operation rules already
  required by GSVA coherence.

The wrappers live with the existing GSVA implementation in `ub_ubc.c` and are
declared through the local UB header used by device models. If the internal
`BusControllerDev` name differs from the public typedef, the header must expose
one canonical type for device-facing calls.

### Device-side GSVA data path

The SSD does not go through ARM TLB fill. After acquire succeeds, it uses the
device-facing GSVA data helpers above.

The helper implementation is responsible for:

```text
1. lookup GSVA route by key and gsva range
2. validate epoch, segment range, p_tag, and cache policy
3. compute route offset from gsva - key.home_va
4. resolve the backing PA/MemoryRegion owned by the route
5. read/write bytes through the QEMU memory API
6. preserve OBMM PA-MESI ordering and fence behavior
```

The SSD must not receive a raw route pointer or raw PA from the route table.
Returning raw address state would let future device code bypass validation.

### CNA allocation

The SSD uses an independent device CNA. It must not reuse the CPU/node CNA.

V1 rule:

```text
device_cna = (node_cna << 16) | (device_type << 8) | instance_id
device_type for SSD = 0x20
instance_id starts at 0
```

Example:

```text
node_cna=0xc4c2
ssd0_cna=0xc4c22000
ssd1_cna=0xc4c22001
```

The QEMU command line may override `cna`. If omitted, QEMU derives it from
`node_cna`, `device_type`, and `instance_id`. Coherence state records the SSD
CNA as the requester and holder.

### Command submission path

V1 command submission is local only:

```text
user ioctl
  -> guest ub_ssd driver
  -> MMIO command window write
  -> MMIO doorbell
  -> QEMU ub-ssd command queue
  -> worker executes command asynchronously
  -> completion MMIO/CQ update
  -> guest wait returns
```

Do not use `ubc_msgq` for guest-to-device command submission in V1. The UB
message layer is still exercised by GSVA coherence when the SSD reads from or
writes to GSVA buffers with remote holders.

Remote command submission is V2. In V1, a guest submits only to an SSD attached
to the same QEMU node. Cross-node tests use remote GSVA buffers, not remote SSD
command submission.

### MMIO layout

The V1 SSD MMIO region is one 4 KiB page.

```text
offset      size     access  meaning
0x000       0x400    W       command slot, contains one ub_ssd_cmd_v1
0x400       0x100    R       completion slot, contains one ub_ssd_cpl_v1
0x500       0x008    R       device_cna
0x508       0x004    R       status
0x50c       0x004    R       error
0x510       0x004    W       doorbell, write 1 to submit command slot
0x514       0x004    W       clear completion, write 1 to release cpl slot
0x518       0x008    R       last_req_id
0x520       0x098    R       stats snapshot
0x5b8       0x008    -       reserved, reads zero, writes ignored
0x5c0       0x004    R       backend_profile
0x5c4       0xa3c    -       reserved, reads zero, writes ignored
```

Status bits:

```text
bit 0: READY
bit 1: BUSY
bit 2: COMPLETION_VALID
bit 3: ERROR
```

Queue depth is exactly 1 in V1. A doorbell write while `BUSY` or
`COMPLETION_VALID` is set returns `SSD_ERR_DEVICE_BUSY` in the completion slot
if the slot is free; otherwise it only sets the error register.

### Guest discovery

The guest discovers the device through Device Tree.

QEMU must add an FDT node when the `virt` machine instantiates `ub-ssd`:

```text
compatible = "ub-sim,ssd-v1"
reg = <mmio base, 0x1000>
ub,node-id = <node-id>
ub,cna = <device-cna>
ub,ubc-phandle = <local ubc phandle>
ub,backend-profile = "memory"
```

V1 does not rely on hard-coded guest physical addresses and does not use
`ub_enum_topo_scan` for driver binding. The UB topology may still log the SSD
CNA for diagnostics, but the Linux platform driver binds through FDT.

### Command execution model

The MMIO doorbell handler must not execute storage operations.

V1 execution model:

```text
doorbell handler:
  validate queue slot shape
  enqueue request
  schedule QEMU bottom half
  return immediately

SSD bottom half:
  pop one command
  run GSVA acquire/read/write/fence steps
  mutate or read the backend only after GSVA validation succeeds
  poll local UBC receive path while waiting for GSVA ACKs
  complete command with stable status
  reschedule itself if more commands remain
```

If a GSVA acquire returns pending, the worker records the pending sequence and
continues through the same bottom half. It must not block the vCPU thread in the
MMIO handler. Timeouts map to `SSD_ERR_COH_TIMEOUT`.

Pending polling rule:

- Do not busy-loop by repeatedly scheduling the BH with no delay.
- V1 uses a `QEMU_CLOCK_VIRTUAL` timer to reschedule the worker every 100 us
  while a command is waiting for GSVA ACKs.
- Each timer tick calls the local UBC receive/progress helper once, then queries
  the pending GSVA operation through the wrapper API.
- Completion, error ACK, or timeout finalizes the command.

The efficient callback path, where UBC RX directly wakes a device worker, is V2.

### Queue depth and concurrency

V1 queue depth is 1.

Rules:

- only one outstanding command exists per SSD instance;
- the command slot cannot be overwritten while `BUSY=1`;
- commands execute serially in the QEMU main loop;
- the memory backend is accessed only from the QEMU main loop;
- no additional worker thread is created;
- host-file backend or AIO backend is V2 and must define its own locking and
  completion handoff before being enabled.

### Topology

V1 scripts should instantiate one local SSD per QEMU node:

```text
2-node: nodeA.ssd0, nodeB.ssd0
4-node: nodeA.ssd0 ... nodeD.ssd0
8-node: nodeA.ssd0 ... nodeH.ssd0
```

Tests may use only nodeA's SSD, but all nodes should be configured the same way
for repeatable multi-node runs.

### UB message subcode

No new `UBC_MSG_SUB_*` value is required for V1 SSD command submission.

The UBC extended header has only 4 bits of sub-message space. GSVA coherence
already uses the viable carrier pattern, so a separate remote device command
carrier would require a transport extension or second-level multiplexer. That is
V2 work.

V1 SSD UB participation is through GSVA coherence messages emitted by the
route/coherence layer.

### V2 remote command transport

Remote command submission requires a UBC transport extension. It must not reuse
`UBC_MSG_SUB_GSVA_COH`.

V2 options:

```text
1. add a second-level device multiplexer under an existing safe carrier;
2. extend the UBC header so device messages have a wider subtype field;
3. add a separate UB device command queue transport outside the 4-bit subcode.
```

Until one of these is implemented, `UB_DEV_MSG_CMD` and `UB_DEV_MSG_CPL` remain
conceptual payload names only.

## UB Endpoint Model

Each SSD instance owns:

```text
device_type = UB_DEV_SSD
device_id
cna
node_id
doorbell_mmio
sq/cq MMIO command window
stats
backend_profile
```

The device participates in UB as a coherence requester/holder through the local
UBC device. A future remote device-command transport may use:

```text
UB_DEV_MSG_CMD
UB_DEV_MSG_CPL
UB_DEV_MSG_ERROR
UB_DEV_MSG_FLUSH
```

Like the NPU design, concrete SSD opcodes live in the command payload. The UB
sub-message space should not grow one value per storage operation. This carrier
is not part of V1.

## Command ABI

V1 command payload:

```c
struct ub_ssd_cmd_v1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_ssd_cna;
    uint32_t flags;
    struct ub_ssd_block_ref_v1 block_ref;
    struct ub_ssd_buffer_desc_v1 buffer;
};

struct ub_ssd_block_ref_v1 {
    uint64_t block_hi;
    uint64_t block_lo;
    uint64_t version;
    uint64_t offset;
    uint64_t bytes;
    uint64_t checksum64;
};

struct ub_ssd_buffer_desc_v1 {
    uint64_t gsva_base;
    uint64_t bytes;
    struct gsva_key_v1 key;
    uint32_t token_id;
    uint32_t token_value;
};
```

Command flags:

```text
SSD_CMD_INJECT_COH_TIMEOUT
```

Completion payload:

```c
struct ub_ssd_cpl_v1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    struct ub_ssd_block_ref_v1 committed_ref;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
};
```

Completion status must use stable values:

```text
SSD_OK
SSD_ERR_BAD_VERSION
SSD_ERR_BAD_OPCODE
SSD_ERR_BAD_BLOCK
SSD_ERR_BAD_DESCRIPTOR
SSD_ERR_TOKEN_DENIED
SSD_ERR_STALE_EPOCH
SSD_ERR_SEGMENT_RETIRED
SSD_ERR_COH_TIMEOUT
SSD_ERR_DEVICE_BUSY
SSD_ERR_CHECKSUM
SSD_ERR_VERSION_CONFLICT
SSD_ERR_SEALED
SSD_ERR_TOMBSTONED
SSD_ERR_BACKEND_IO
SSD_ERR_BAD_SNAPSHOT
```

### ABI ownership

`struct gsva_key_v1` is not a device-specific type. The SSD UAPI includes the
canonical GSVA UAPI definitions and embeds that structure directly. The token is
carried as the canonical GSVA V1 `token_id` and `token_value` pair used by GSVA
segment records and ioctl events. QEMU converts only at the boundary required by
existing internal headers; field meaning and validation rules are identical to
GSVA V1.

Do not define SSD-specific key or token variants with different layout.

### Block ID and version rules

`block_hi:block_lo` is caller-specified in V1.

Reason:

- Lingqu Block treats block identity as durable payload identity;
- caller-specified IDs make restart tests deterministic;
- hash-derived or device-allocated IDs can be layered above this ABI.

Recommended caller policy:

```text
block_hi = checksum64(namespace || object_id)
block_lo = checksum64(payload_kind || logical_index)
```

The SSD validates only that the 128-bit block ID is non-zero.

Version rules:

- `block_ref.version` in a write command is `expected_version`.
- `expected_version=0` means create the first version and fail if the block
  already has a committed or sealed version.
- `expected_version=N` means append version `N + 1` only if the latest durable
  version is `N`.
- completion returns the committed version in `committed_ref.version`.
- conflicts return `SSD_ERR_VERSION_CONFLICT` and do not mutate the backend.

Failure-injection rule:

- `SSD_CMD_INJECT_COH_TIMEOUT` completes with `SSD_ERR_COH_TIMEOUT` after
  command version validation and before any GSVA data access or backend
  mutation. The injection is V1 test-only and must not publish a committed block
  version.

Read rules:

- `version=0` reads the latest committed or sealed version.
- `version=N` reads exact version `N`.
- tombstoned, quarantined, missing, or checksum-mismatched records fail
  explicitly.

## GSVA/OBMM Access Rules

WRITE from GSVA buffer to durable block:

```text
validate command and block ref
validate GSVA descriptor and read token
GSVA ReadAcquire(key, ssd_cna)
read bytes through OBMM data path
compute checksum
write/version block in backend
complete with committed block ref
```

READ from durable block to GSVA buffer:

```text
validate block ref and checksum
validate GSVA descriptor and write token
GSVA WriteAcquire(key, ssd_cna)
read durable bytes from backend
write bytes through OBMM data path
GSVA/OBMM fence for written range
complete with checksum
```

Rules:

- The SSD CNA is the requester and holder in GSVA coherence state.
- A READ into GSVA output must not write data until WriteAcquire succeeds.
- A WRITE from GSVA input must not persist data until ReadAcquire succeeds.
- Flush and seal must be ordered after all previous writes for the same block.
- Retired or stale GSVA descriptors fail before backend mutation.
- Failed backend writes must not publish a committed block version.

## Lingqu Block Alignment

Lingqu Block is the durable payload model for the SSD.

The SSD backend should use the same logical record rules:

```text
block identity
version
durable_state = Committed | Sealed | Tombstoned | Quarantined
bytes
checksum
writer
metadata
```

V1 backend options:

```text
memory backend: deterministic tests and fast smoke runs
host-file backend: restart tests and larger payloads
LingquDurableSim backend: direct alignment with sim-services durable snapshot
```

The preferred first implementation is a memory backend with export/import
snapshot support that matches `LingquBlockSimSnapshot`. A later implementation
can replace it with direct `LingquDurableSim` calls when the guest/device
boundary is stable.

V1 memory backend structure:

```text
GHashTable key = (block_hi, block_lo)
  -> UbSsdBlockChain

UbSsdBlockChain
  block_hi
  block_lo
  versions: ordered array of UbSsdBlockRecord

UbSsdBlockRecord
  version
  durable_state
  bytes
  checksum64
  writer_cna
  metadata_flags
```

The exported snapshot should be structurally compatible with
`LingquBlockSimSnapshot`: versioned records, durable state, byte length,
checksum, writer, and metadata. It does not need to share Rust structs with
`sim-services::durable` in V1 because the QEMU model is C, but the JSON/binary
schema must preserve the same fields and validation rules.

Block rules:

- `BLOCK_WRITE` creates a new committed version unless `expected_version`
  conflicts.
- `BLOCK_READ` verifies range and checksum.
- `BLOCK_SEAL` prevents future overwrites.
- `BLOCK_TOMBSTONE` appends a tombstone version.
- Missing blocks, sealed writes, checksum mismatch, and version conflict are
  hard structured failures.

## Lingqu DFS Alignment

The SSD does not own human-readable paths. Lingqu DFS owns namespace,
manifests, and audit records.

Recommended paths:

```text
/lingqu/block/devices/<ssd-id>.json
/lingqu/block/objects/<object-id>.json
/lingqu/block/audit/<ssd-id>.log
/lingqu/memory/execution-artifacts/<artifact-id>.json
/lingqu/memory/prefix-cache/<artifact-id>.json
```

DFS manifests may reference SSD-produced Lingqu Block refs:

```text
SsdBlockManifest
  object_id
  block_refs: [LingquBlockPayloadRef]
  producer_device
  checksum64
  created_at
  metadata
```

Boundary rule:

- SSD commands produce or consume Lingqu Block payload refs.
- DFS publishes names and manifests that point at those refs.
- Guest runtime, Memory Service, or Object Service decides when to publish DFS
  records.
- The SSD must not create ad hoc JSON registry files outside DFS.

This preserves the durable design rule that DFS and Block are peers: DFS names
and versions manifests; Block owns bytes.

## Guest Interface

V1 should expose a simple test-first interface:

```text
/dev/ub_ssd0

ioctl(UB_SSD_SUBMIT, struct ub_ssd_cmd_v1)
ioctl(UB_SSD_WAIT, struct ub_ssd_cpl_v1)
ioctl(UB_SSD_QUERY, struct ub_ssd_query_v1)
ioctl(UB_SSD_EXPORT_SNAPSHOT, ...)
ioctl(UB_SSD_IMPORT_SNAPSHOT, ...)
```

The user-space test flow for write/read:

```text
1. allocate GSVA buffer through existing manager/OBMM path
2. mmap buffer with MAP_GSVA
3. fill buffer with test bytes
4. submit SSD BLOCK_WRITE with GSVA descriptor and token
5. receive committed LingquBlockPayloadRef-compatible completion
6. clear or replace buffer
7. submit SSD BLOCK_READ into a GSVA output buffer
8. verify output bytes and checksum from another node
9. optionally publish DFS manifest referencing the committed block ref
```

Do not start by integrating Linux block layer. The char-device ioctl path keeps
the first acceptance loop focused on UB, GSVA, OBMM, and Lingqu durable
semantics.

Snapshot export/import in V1:

- format is JSON for readability and alignment with current Lingqu durable
  simulation snapshots;
- payload bytes are hex or base64 encoded;
- import validates block IDs, versions, durable states, byte lengths, and
  checksums before replacing the live backend;
- a failed import leaves the existing backend unchanged.

## Required Tests

Acceptance scripts should cover 2/4/8-node topologies.

```text
ssd_block_write_read_gsva
ssd_block_write_from_nodeA_read_to_nodeB
ssd_seal_rejects_overwrite
ssd_tombstone_rejects_read
ssd_bad_token_denied
ssd_stale_epoch_denied
ssd_retired_segment_denied
ssd_checksum_mismatch_denied
ssd_dfs_manifest_refs_block_payload
```

Required log evidence:

```text
UB_SSD_CMD
UB_SSD_CPL
GSVA_ROUTE
GSVA_COH
GSVA_TLB
OBMM_COH_*
LINGQU_BLOCK_WRITE
LINGQU_BLOCK_READ
LINGQU_DFS_MANIFEST
```

Negative log rules:

```text
no GVA_TCG_TRANSLATE in default arm_mmu mode
no backend write after GSVA validation failure
no committed block after checksum failure
no synthetic payload for missing block
```

## Recovery and Stats

V1 recovery rule:

- Device-held GSVA state is represented by the SSD CNA in the existing GSVA
  coherence object.
- Manager recovery treats an SSD holder like any other holder CNA.
- If the QEMU node hosting the SSD exits, simulator-level recovery may
  force-retire or rebuild state through the existing manager recovery path.
- The SSD backend snapshot is durable state; GSVA holder state is runtime state.

V1 stats:

```text
ssd_cmd_total
ssd_cmd_completed
ssd_cmd_failed
ssd_block_write
ssd_block_read
ssd_block_seal
ssd_block_tombstone
ssd_flush
ssd_stat
ssd_bytes_read_from_gsva
ssd_bytes_written_to_gsva
ssd_bytes_written_to_backend
ssd_bytes_read_from_backend
ssd_token_denied
ssd_stale_epoch
ssd_retired_segment
ssd_version_conflict
ssd_coh_timeout
ssd_checksum_error
```

Latency histograms are V2. V1 may log timestamps, but acceptance must key off
stable counters and completion status.

## Implementation Plan

1. Add QEMU `ub_ssd` skeleton with UB identity, MMIO doorbell, command parser,
   completion, stats, and in-memory backend.
2. Add guest `ub_ssd` ioctl definitions and a small test driver or app helper.
3. Implement command echo and completion without backend mutation.
4. Add GSVA descriptor validation and token checking.
5. Add `BLOCK_WRITE` and `BLOCK_READ` through GSVA ReadAcquire/WriteAcquire and
   OBMM data-layer access.
6. Add checksum validation, version conflict, seal, and tombstone state.
7. Add snapshot export/import compatible with Lingqu Block simulation records.
8. Add DFS manifest publication tests from user space.
9. Add deterministic timeout failure injection through
   `SSD_CMD_INJECT_COH_TIMEOUT` so acceptance does not depend on a real fabric
   timeout.
9. Add optional host-file backend after deterministic memory backend passes.

## V1 Acceptance Criteria

- SSD commands complete over UB endpoint control path.
- SSD WRITE persists bytes only after GSVA ReadAcquire succeeds.
- SSD READ writes GSVA buffers only after GSVA WriteAcquire succeeds.
- SSD requester CNA appears in GSVA coherence state transitions.
- Token, epoch, retire, checksum, seal, tombstone, and version conflict negative
  tests fail explicitly.
- Data written from one node can be read into a GSVA buffer visible to another
  node.
- Committed payload refs can be named by Lingqu DFS manifests without
  duplicating durable state inside the SSD.
