# UB-Attached Semantic NPU Simulation Design

## Goal

This document defines the first simulator design for a UB-attached NPU endpoint.
The goal is not to model a real NPU microarchitecture. The goal is to prove that
an accelerator device can join the existing UB, GSVA, and OBMM protocol stack as
a first-class endpoint.

The V1 NPU must demonstrate:

- the NPU has its own UB identity, route, and CNA;
- guest software can submit work to the NPU through a device command queue;
- the NPU reads and writes buffers described by GSVA segment descriptors;
- every NPU data access is protected by GSVA token, epoch, retire, fence, and
  coherence rules;
- NPU execution artifacts can be named and recovered through Lingqu DFS and
  Lingqu Block without making the NPU own a private durability model.

## Non-Goals

- Do not implement a real tensor compiler, runtime, scheduler, or kernel ISA in
  V1.
- Do not simulate cycle-accurate NPU pipelines, SRAM banks, DMA engines, or HBM.
- Do not bypass GSVA/OBMM by writing directly through host pointers.
- Do not store durable output bytes in an NPU-private registry.
- Do not make Lingqu DFS or Lingqu Block depend on NPU-specific metadata.

## Architecture

The NPU is a UB endpoint behind the existing UB fabric.

```text
guest process
  -> /dev/ub_npu0 ioctl
  -> guest kernel ub_npu driver
  -> QEMU ub-npu SysBus/MMIO device
  -> local command worker
  -> GSVA route/coherence
  -> UB Link for GSVA coherence messages when remote holders are involved
  -> OBMM data backend
```

The NPU is a semantic device: it executes small deterministic operations over
GSVA buffers so the simulator can verify memory semantics, ordering, and failure
behavior.

V1 operation set:

```text
NPU_OP_MEMCOPY
NPU_OP_FILL
NPU_OP_VECTOR_ADD_U32
NPU_OP_CHECKSUM64
```

These operations are intentionally simple. They make input/output validation
obvious while exercising read ownership, write ownership, fence, completion, and
negative paths.

## V1 Implementation Decisions

These decisions are fixed for the first code path.

### QEMU device type

`ub-npu` is a QEMU `SysBusDevice`, not a PCI device.

Reason:

- current `ub_sim` guest machines already use platform-style UB devices;
- V1 does not need PCI enumeration, BAR sizing, MSI-X, or config space;
- SysBus MMIO keeps the first kernel driver and QEMU model small.

The device is created with explicit QEMU `-device` properties:

```text
-device ub-npu,id=npu0,node-id=0,cna=0xc4c20010,ubc=/machine/peripheral/ubc0
```

The `ubc` property is a link to the local `UbUbcState`. QEMU realization fails
if the linked UBC device is missing or not GSVA-capable. The NPU does not create
its own route table; it calls the GSVA route/coherence helpers owned by the
local UBC device.

### QEMU GSVA access API

The NPU must not include or dereference the GSVA static globals in
`ub_ubc.c`. In particular, it must not reach into `g_gsva_routes` or
`g_gsva_coh` directly.

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

The NPU does not go through ARM TLB fill. After acquire succeeds, it uses the
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

The NPU must not receive a raw route pointer or raw PA from the route table.
Returning raw address state would let future device code bypass validation.

### CNA allocation

The NPU uses an independent device CNA. It must not reuse the CPU/node CNA.

V1 rule:

```text
device_cna = (node_cna << 16) | (device_type << 8) | instance_id
device_type for NPU = 0x10
instance_id starts at 0
```

Example:

```text
node_cna=0xc4c2
npu0_cna=0xc4c21000
npu1_cna=0xc4c21001
```

The QEMU command line may override `cna`. If omitted, QEMU derives it from
`node_cna`, `device_type`, and `instance_id`. Coherence state records the device
CNA as the requester and holder, so collisions with CPU CNAs or other devices
are fatal configuration errors.

### Command submission path

V1 command submission is local only:

```text
user ioctl
  -> guest ub_npu driver
  -> MMIO command window write
  -> MMIO doorbell
  -> QEMU ub-npu command queue
  -> worker executes command asynchronously
  -> completion MMIO/CQ update
  -> guest wait returns
```

Do not use `ubc_msgq` for guest-to-device command submission in V1. The UB
message layer is still exercised by GSVA coherence when the NPU touches a GSVA
segment with remote holders.

Remote command submission is a V2 feature. In V1, a guest submits only to an NPU
attached to the same QEMU node. Cross-node behavior is tested by placing input
or output GSVA holders on other nodes, not by sending the command itself to a
remote NPU.

### MMIO layout

The V1 NPU MMIO region is one 4 KiB page.

```text
offset      size     access  meaning
0x000       0x400    W       command slot, contains one ub_npu_cmd_v1
0x400       0x100    R       completion slot, contains one ub_npu_cpl_v1
0x500       0x008    R       device_cna
0x508       0x004    R       status
0x50c       0x004    R       error
0x510       0x004    W       doorbell, write 1 to submit command slot
0x514       0x004    W       clear completion, write 1 to release cpl slot
0x518       0x008    R       last_req_id
0x520       0x080    R       stats snapshot
0x5a0       0xa60    -       reserved, reads zero, writes ignored
```

Status bits:

```text
bit 0: READY
bit 1: BUSY
bit 2: COMPLETION_VALID
bit 3: ERROR
```

Queue depth is exactly 1 in V1. A doorbell write while `BUSY` or
`COMPLETION_VALID` is set returns `NPU_ERR_DEVICE_BUSY` in the completion slot
if the slot is free; otherwise it only sets the error register.

### Guest discovery

The guest discovers the device through Device Tree.

QEMU must add an FDT node when `ub-npu` is realized:

```text
compatible = "ub-sim,npu-v1"
reg = <mmio base, 0x1000>
ub,node-id = <node-id>
ub,cna = <device-cna>
ub,ubc-phandle = <local ubc phandle>
```

V1 does not rely on hard-coded guest physical addresses and does not use
`ub_enum_topo_scan` for driver binding. The UB topology may still log the NPU
CNA for diagnostics, but the Linux platform driver binds through FDT.

### Command execution model

The MMIO doorbell handler must not run the command body.

V1 execution model:

```text
doorbell handler:
  validate queue slot shape
  enqueue request
  schedule QEMU bottom half
  return immediately

NPU bottom half:
  pop one command
  run GSVA acquire/read/write/fence steps
  poll local UBC receive path while waiting for GSVA ACKs
  complete command with stable status
  reschedule itself if more commands remain
```

If a GSVA acquire returns pending, the worker records the pending sequence and
continues progress through the same QEMU bottom half. It must not block the vCPU
thread in the MMIO handler. Timeouts map to `NPU_ERR_COH_TIMEOUT`.

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

- only one outstanding command exists per NPU instance;
- the command slot cannot be overwritten while `BUSY=1`;
- commands execute serially in the QEMU main loop;
- no additional worker thread is created;
- later queue-depth expansion must preserve request ordering for commands that
  touch overlapping GSVA ranges.

### Topology

V1 scripts should instantiate one local NPU per QEMU node:

```text
2-node: nodeA.npu0, nodeB.npu0
4-node: nodeA.npu0 ... nodeD.npu0
8-node: nodeA.npu0 ... nodeH.npu0
```

Tests may use only nodeA's NPU, but all nodes should be configured the same way
so cross-node validation does not depend on a special topology.

### UB message subcode

No new `UBC_MSG_SUB_*` value is required for V1 NPU command submission.

The existing UBC extended header has only 4 bits of sub-message space and GSVA
coherence already uses the only viable carrier pattern. Allocating a new carrier
for device commands would require a transport header extension or a second-level
multiplexer. That is explicitly V2.

V1 NPU UB participation is through GSVA coherence messages already emitted by
the route/coherence layer.

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

Each NPU instance owns:

```text
device_type = UB_DEV_NPU
device_id
cna
node_id
doorbell_mmio
sq/cq MMIO command window
stats
```

The device participates in UB as a coherence requester/holder through the local
UBC device. A future remote device-command transport may use:

```text
UB_DEV_MSG_CMD
UB_DEV_MSG_CPL
UB_DEV_MSG_ERROR
UB_DEV_MSG_FENCE
```

Do not allocate one UB subcode per NPU opcode. Use one device command carrier
and put the concrete NPU opcode in the command payload. This avoids pressure on
the existing 4-bit UBC sub-message space. This carrier is not part of V1.

## Command ABI

V1 command payload:

```c
struct ub_npu_cmd_v1 {
    uint32_t version;
    uint32_t opcode;
    uint64_t req_id;
    uint32_t source_cna;
    uint32_t target_npu_cna;
    uint32_t flags;
    uint32_t desc_count;
    struct ub_npu_buffer_desc_v1 descs[4];
    uint64_t scalar0;
    uint64_t scalar1;
};

struct ub_npu_buffer_desc_v1 {
    uint32_t role;
    uint32_t access;
    uint64_t gsva_base;
    uint64_t bytes;
    struct gsva_key_v1 key;
    struct gsva_token_v1 token;
};
```

Descriptor roles:

```text
NPU_BUF_INPUT
NPU_BUF_WEIGHT
NPU_BUF_OUTPUT
NPU_BUF_SCRATCH
```

Access values:

```text
NPU_ACCESS_READ
NPU_ACCESS_WRITE
NPU_ACCESS_READ_WRITE
```

Completion payload:

```c
struct ub_npu_cpl_v1 {
    uint32_t version;
    uint32_t status;
    uint64_t req_id;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t checksum64;
    uint64_t error_detail;
};
```

Completion status must use stable values:

```text
NPU_OK
NPU_ERR_BAD_VERSION
NPU_ERR_BAD_OPCODE
NPU_ERR_BAD_DESCRIPTOR
NPU_ERR_TOKEN_DENIED
NPU_ERR_STALE_EPOCH
NPU_ERR_SEGMENT_RETIRED
NPU_ERR_COH_TIMEOUT
NPU_ERR_DEVICE_BUSY
```

### ABI ownership

`struct gsva_key_v1` and `struct gsva_token_v1` are not device-specific types.
The NPU UAPI includes the canonical GSVA UAPI definitions and embeds those
structures directly. QEMU converts only at the boundary required by existing
internal headers; field meaning and validation rules are identical to GSVA V1.

Do not define `ub_npu_gsva_key` or `ub_npu_token` aliases with different layout.

### Opcode field rules

Descriptor count and scalar usage are fixed:

```text
opcode                 desc_count  desc roles                         scalar0              scalar1
NPU_OP_MEMCOPY         2           INPUT, OUTPUT                      unused               unused
NPU_OP_FILL            1           OUTPUT                             fill_u64             unused
NPU_OP_VECTOR_ADD_U32  3           INPUT, INPUT, OUTPUT               element_count_u32    unused
NPU_OP_CHECKSUM64      1           INPUT                              unused               unused
```

Validation rules:

- `desc_count` must be between 1 and 4.
- Extra descriptors beyond the required count are rejected.
- Required roles must appear in order.
- `bytes` for `MEMCOPY` is `min(input.bytes, output.bytes)` and both sizes must
  match unless `NPU_CMD_ALLOW_TRUNCATE` is set.
- `FILL` writes exactly `output.bytes`.
- `VECTOR_ADD_U32` requires `element_count_u32 * 4` bytes available in both
  inputs and the output.
- `CHECKSUM64` reads exactly `input.bytes` and returns the checksum in
  completion.
- Any descriptor outside its GSVA segment range returns
  `NPU_ERR_BAD_DESCRIPTOR`.

## GSVA/OBMM Access Rules

The NPU must access memory through the same semantic path as a CPU node.

Read flow:

```text
validate descriptor
validate token for read
GSVA ReadAcquire(key, npu_cna)
OBMM/PA data-layer read
optional checksum update
```

Write flow:

```text
validate descriptor
validate token for write
GSVA WriteAcquire(key, npu_cna)
OBMM/PA data-layer write
GSVA/OBMM fence for written range
```

Rules:

- The NPU CNA is the requester and holder in GSVA coherence state.
- Descriptor `epoch` must match the active segment epoch.
- Retired tombstones reject all NPU commands before touching data.
- Token rotation while a command is pending must either fail the command with a
  stable error or force a retry path; it must not silently continue with an old
  token.
- Data movement may be implemented inside QEMU for V1, but it must call the
  same GSVA route/coherence and OBMM data helpers used by CPU and guest paths.
- A command with both reads and writes must acquire all read descriptors before
  acquiring write descriptors. If a later acquire fails, no output write is
  committed.

For `MEMCOPY`, the implementation reads input bytes into a temporary QEMU heap
buffer after the input ReadAcquire succeeds, then performs output WriteAcquire,
writes the temporary buffer through `ubc_gsva_device_write()`, and fences the
output range. This avoids exposing source route internals to the NPU model.

## Lingqu DFS/Block Alignment

The NPU does not own durable namespace or durable bytes.

Lingqu DFS owns:

```text
/lingqu/npu/jobs/<job-id>.json
/lingqu/npu/execution-artifacts/<artifact-id>.json
/lingqu/memory/execution-artifacts/<artifact-id>.json
```

Lingqu Block owns durable payload bytes when NPU input, weight, output, or
artifact data needs to survive beyond the hot GSVA runtime window.

The NPU command may carry optional provenance fields in a higher-level guest
runtime manifest, but the hardware-facing command uses GSVA descriptors only.

Recommended manifest shape:

```text
NpuExecutionManifest
  job_id
  opcode
  input_placements: [GsvaPlacement | LingquBlockPayloadRef]
  output_placements: [GsvaPlacement]
  output_block_refs: [LingquBlockPayloadRef]
  checksum64
  status
```

Boundary rule:

- GSVA/OBMM is the hot runtime data plane.
- Lingqu Block is the durable payload plane.
- Lingqu DFS is the durable naming, manifest, and audit plane.
- The NPU returns completion metadata; guest runtime or Memory Service publishes
  DFS/Block records after completion.

This keeps accelerator execution separate from durable catalog ownership and
matches the existing Lingqu durable simulation design.

## Guest Interface

V1 should expose a simple test-first interface:

```text
/dev/ub_npu0

ioctl(UB_NPU_SUBMIT, struct ub_npu_cmd_v1)
ioctl(UB_NPU_WAIT, struct ub_npu_wait_v1)
ioctl(UB_NPU_QUERY, struct ub_npu_query_v1)
```

The user-space test flow:

```text
1. allocate GSVA input/output segments through existing manager/OBMM path
2. mmap both segments with MAP_GSVA
3. initialize input buffer
4. submit NPU command with GSVA descriptors and tokens
5. wait for completion
6. map/read output from another node and verify bytes
7. optionally publish output payload to Lingqu Block and manifest to Lingqu DFS
```

Do not start with a Linux accelerator framework integration. The char-device
ioctl path keeps the first acceptance loop small and compatible with current
GSVA test structure.

## Required Tests

Acceptance scripts should cover 2/4/8-node topologies.

```text
npu_memcopy_gsva
npu_fill_gsva
npu_vector_add_gsva
npu_checksum_gsva
npu_bad_token_denied
npu_stale_epoch_denied
npu_retired_segment_denied
npu_token_rotate_pending
npu_output_publish_block_dfs
```

Required log evidence:

```text
UB_NPU_CMD
UB_NPU_CPL
GSVA_ROUTE
GSVA_COH
GSVA_TLB
OBMM_COH_*
```

Negative log rules:

```text
no GVA_TCG_TRANSLATE in default arm_mmu mode
no direct host pointer bypass marker
no success completion after token denial
no success completion after retired segment rejection
```

## Recovery and Stats

V1 recovery rule:

- Device-held GSVA state is represented by the device CNA in the existing GSVA
  coherence object.
- Manager recovery treats an NPU holder like any other holder CNA.
- If the QEMU node hosting the NPU exits, the current simulator-level recovery
  may force-retire or rebuild state through the existing manager recovery path.
- No NPU-private recovery log is added in V1.

V1 stats:

```text
npu_cmd_total
npu_cmd_completed
npu_cmd_failed
npu_opcode_memcopy
npu_opcode_fill
npu_opcode_vector_add_u32
npu_opcode_checksum64
npu_bytes_read
npu_bytes_written
npu_token_denied
npu_stale_epoch
npu_retired_segment
npu_coh_timeout
```

Latency histograms are V2. V1 may log start/end timestamps for debugging, but
acceptance must key off counters and stable completion status.

## Implementation Plan

1. Add QEMU `ub_npu` skeleton with UB identity, MMIO doorbell, command parser,
   completion, and stats.
2. Add guest `ub_npu` ioctl definitions and a small test driver or app helper.
3. Implement command echo and completion without data access.
4. Add GSVA descriptor validation and token checking.
5. Add read/write helpers that route through GSVA coherence and OBMM data-layer
   access.
6. Implement `MEMCOPY`, `FILL`, `VECTOR_ADD_U32`, and `CHECKSUM64`.
7. Add failure injection for bad token, stale epoch, retired segment, timeout,
   and device busy.
8. Add Lingqu DFS/Block manifest publication tests from user space.

## V1 Acceptance Criteria

- NPU commands complete over UB endpoint control path.
- NPU reads and writes GSVA buffers without CPU data copy.
- NPU requester CNA appears in GSVA coherence state transitions.
- Token, epoch, retire, and coherence timeout negative tests fail explicitly.
- Output written by the NPU is visible to a different node through MAP_GSVA.
- NPU output can be published as Lingqu Block payload and named by Lingqu DFS
  manifest without duplicating durable state inside the NPU.
