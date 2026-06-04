# GSVA Address Management Design on OBMM shmem

## 1. Goal

This document defines `GSVA` as the strict identity profile of the `GVA Manager`
architecture in `docs/sim_gva_simulation_design.md`. In this mode, global shared
virtual addresses are managed as one global address space:

```text
user_va == uba == home_va
```

The key correction from a simple QEMU alias design is this: GSVA cannot be made
correct by hiding address differences behind a backend mapping table. The
address equality must be visible to the sub-domain guest OS in `ub_sim`, OBMM,
the user process, and the GVA control plane.

Therefore GSVA needs an independent `GVA Manager` component running on every OS
inside the UB-connected supernode. These managers coordinate through OBMM shared
memory MPMC queues during bootstrap, reserve a common global virtual address
range, register that range with the guest kernel and OBMM address managers, and
then serve GSVA allocations from that reserved range.

Layering with the GVA design:

```text
GVA Manager
  -> allocates GSVA segment from reserved range
  -> produces ub_gva_map_req {
       local_va=gsva_base,
       uba_base=gsva_base,
       pte_offset=0,
       address_profile=gsva_identity
     }
      -> GVA Simulation Layer
          -> MMU.S3 ma_table / NoC mp_table
              -> QEMU backend / UB Link
```

GSVA does not replace the GVA Simulation Layer. It provides the global address
management input that the GVA Simulation Layer consumes.

## 2. Direct Conclusion

The right architecture is:

```text
GVA Manager on each OS
  <-> OBMM MPMC manager queue
      -> bootstrap consensus for GSVA reserved address range
          -> guest kernel reserves the range from normal userspace allocation
          -> OBMM marks the range as GSVA-owned
              -> applications allocate/map GSVA through explicit API or mmap flag
                  -> OBMM/GVA control plane programs GVA route
                      -> QEMU GVA/S3/NoC backend executes access
```

The wrong architecture is:

```text
public GSVA address -> QEMU private actual_home_uba alias
```

That approach may be useful as a debug aid, but it is not GSVA. It bypasses the
real management problem: the same numeric address must be allocated, protected,
and recognized consistently by every OS and by OBMM.

## 3. Design Principles

1. GSVA address equality is an invariant, not a log formatting convention.
2. GSVA allocation must be owned by a distributed GVA management plane, not by
   ad hoc application `mmap(NULL, ...)`.
3. The guest kernel must know the reserved GSVA aperture so the normal virtual
   memory allocator does not consume it.
4. OBMM must know the reserved GSVA aperture so export/import and shmdev mmap
   can validate GSVA ranges.
5. GVA Manager nodes must coordinate through the existing OBMM data-sharing
   substrate, specifically MPMC queues, because this is the current UB-connected
   control channel that can be validated in QEMU.
6. Strict GSVA never falls back to relocation. If the reserved address cannot be
   honored, the session fails.

## 4. Definitions

`GVA Manager`
: A per-OS service or kernel-assisted daemon responsible for GSVA bootstrap,
  global address allocation, lifetime tracking, and OBMM/GVA programming.

`GSVA aperture`
: The globally agreed virtual address range reserved for GSVA allocations.
  Normal application mappings must not be placed there except through GSVA APIs.
  In `docs/sim_gva_simulation_design.md` this is called `GSVA reserved VA
  aperture` to distinguish it from a QEMU `MemoryRegion` dispatch aperture.

`GSVA segment`
: A home-owned subrange allocated from the GSVA aperture and backed by OBMM
  export/import mappings.

`home_va`
: The address where the home process maps the segment. In GSVA this equals the
  segment's `gsva_base`.

`user_va`
: The address where any process maps the segment. In GSVA this also equals
  `gsva_base`.

`uba`
: The UB address programmed into the GVA control plane. In GSVA this equals
  `gsva_base`.

`actual backing`
: The physical or host-side backing selected by the current OBMM implementation.
  It is implementation detail below the GSVA/GVA address contract.

## 5. Current Baseline

Current `ub_sim` already has useful building blocks:

1. OBMM export/import can create local and remote shared-memory objects.
2. `/dev/obmm_shmdev<MEM_ID>` supports userspace `mmap`.
3. OBMM import callback already programs the SIM_DEC/QEMU backend.
4. The OBMM queue demo already validates owner-sharded SPSC/SPMC/MPSC/MPMC queue
   layouts over exported/imported OBMM regions.
5. FM bootstrap can distribute per-node records.
6. The GVA design document defines explicit `GVA map`, `MMU.S3 ma_table`, and
   `NoC mp_table` semantics for QEMU.

Current gaps for GSVA:

1. Existing helper code maps shmem with `mmap(NULL, ...)`, so user VA is chosen
   by the kernel.
2. Guest kernel has no globally reserved GSVA aperture.
3. OBMM does not validate that a shmdev mmap address belongs to a GSVA range.
4. OBMM export returns an address selected by its current UMMU/DMA path; it is
   not driven by a distributed GSVA allocator.
5. Bootstrap records do not describe a GSVA address session.
6. There is no per-OS GVA Manager.

## 6. Target Architecture

### 6.1 Component View

```text
Application
  -> libgva / mmap(GSVA flag)
      -> GVA Manager local API
          -> guest kernel GSVA aperture registry
          -> OBMM GSVA allocator/mapper
          -> GVA control plane
              -> QEMU MMU.S3 / NoC model
                  -> UB Link
                      -> home memory

GVA Manager peers
  <-> OBMM MPMC queues
      -> bootstrap agreement
      -> allocation announcements
      -> revoke/unmap notifications
      -> health and generation fencing
```

### 6.2 Address Contract

For every active GSVA segment:

```text
segment.gsva_base == userspace mmap address on every participant
segment.gsva_base == public UBA in GVA map
segment.gsva_base == home userspace mmap address
pte_offset        == 0
```

The current OBMM physical backing may still be implemented by existing kernel
and QEMU mechanisms, but those mechanisms must be programmed as a consequence of
the GSVA allocation. They cannot expose a different architectural address.

## 7. Bootstrap Protocol

### 7.1 Manager Queue Setup

Before GSVA can serve applications, every node creates a small OBMM-backed
manager control plane:

```text
1. each node exports a manager control region
2. nodes exchange manager export descriptors through existing OBMM bootstrap
3. each node imports peer manager regions
4. each node initializes MPMC queues:
   manager_queue[dst][src] = control messages from src to dst
5. all managers enter GSVA bootstrap generation G
```

This reuses the queue ownership rules from
`docs/obmm_shared_memory_pool_lockless_queue_design.md`: each queue or lane has
single-writer metadata where possible, and cross-node writes use OBMM import
non-cacheable mappings unless a later cache-coherent policy is explicitly added.

### 7.2 Dependency on OBMM Bootstrap

GSVA manager bootstrap has a hard dependency on the existing OBMM bootstrap
service. The dependency is only for L0 discovery and queue bring-up; it is not
the GSVA address allocation protocol.

The bootstrap stack is:

```text
OBMM bootstrap
  -> discovers manager control regions
      -> imports peer manager regions
          -> brings up OBMM MPMC manager queues
              -> runs GVA Manager bootstrap
                  -> commits GSVA reserved VA aperture
```

Required order:

1. `obmm.ko` and `ub-sim-decoder.ko` must be loaded before `gva_manager` starts.
2. Each manager exports one small OBMM manager control region.
3. Each manager publishes that region through `OBMM_CMD_BOOTSTRAP_PUBLISH`.
4. Each manager waits on `OBMM_CMD_BOOTSTRAP_LOOKUP` until all peer manager
   control-region descriptors are visible for the same generation.
5. Each manager imports peer manager regions and initializes MPMC queue lanes.
6. Only after all manager queues are ready can GSVA aperture proposal begin.

The existing `struct obmm_bootstrap_record` is sufficient for the L0 manager
control region because it carries:

```text
{export_mem_id, remote_uba, size, generation, node_id, node_count,
 export_cna, token_id}
```

It is not sufficient as the long-term GSVA segment descriptor, because GSVA
segments need `segment_id`, `gsva_base`, address profile, lease state, and
retire generation. Those records belong to the GVA Manager protocol carried over
the MPMC queues, not to OBMM bootstrap v1.

Failure semantics:

1. If OBMM bootstrap publish/lookup fails, GSVA manager bootstrap fails before
   any GSVA aperture is proposed.
2. If a peer manager control region cannot be imported, the generation fails.
3. If manager queues cannot be initialized, the generation fails.
4. A UDP or host-side fallback may be useful for debug, but it is not an
   acceptance path for GSVA. The acceptance path must prove OBMM bootstrap and
   OBMM MPMC manager queues both work.

### 7.3 Aperture Proposal

Each manager proposes local constraints:

```c
struct gva_mgr_aperture_proposal {
    uint64_t generation;
    uint32_t node_id;
    uint32_t node_count;
    uint64_t min_base;
    uint64_t max_base;
    uint64_t size;
    uint64_t alignment;
    uint64_t forbidden_hash;
    uint32_t va_bits;
    uint32_t flags;
};
```

`forbidden_hash` summarizes ranges that the local OS cannot reserve because of
kernel layout, existing process mappings, configured guard holes, or known UB
windows. The first implementation may keep the forbidden set simple and use a
fixed list in the manager config; later versions can query the kernel.

### 7.4 Consensus

The bootstrap consensus is deterministic:

```text
candidate_base = first aligned range accepted by every proposal
candidate_size = min(requested sizes) or configured exact size
```

All managers independently compute the same result from the same proposal set.
The chosen range is accepted only after every node has acknowledged:

```text
GVA_MGR_MSG_APERTURE_ACCEPT {
    generation,
    node_id,
    base,
    size,
    layout_hash
}
```

If any node rejects, the generation fails. A later generation may retry with a
different policy.

### 7.5 Kernel Reservation

After consensus, each manager asks its local guest kernel to reserve the range:

```c
struct gva_reserve_range_req {
    uint64_t generation;
    uint64_t base;
    uint64_t size;
    uint64_t flags;
};
```

The kernel must make the range visible to the normal VA allocator and reject
ordinary mappings that collide with it. Implementation options:

1. A GSVA misc device ioctl such as `GVA_CMD_RESERVE_RANGE`.
2. A new OBMM ioctl if GSVA remains owned by OBMM in early bring-up.
3. A debugfs/sysfs control only for initial experiments, not for long-term API.

The reservation must be process-aware:

1. System/global state records the aperture as GSVA-owned.
2. Each GSVA-capable process must reserve the same hole in its `mm`.
3. The manager should reserve the aperture early in process startup, before
   application allocations fragment the address space.

For the first implementation, the userspace helper can still perform:

```text
mmap(base, size, PROT_NONE,
     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
     -1, 0)
```

but this is only a per-process reservation. It is not sufficient by itself. The
kernel/OBMM registry is required so later GSVA mmap calls can be validated and
ordinary OBMM mappings cannot accidentally use GSVA space.

### 7.6 OBMM Awareness

OBMM needs a GSVA aperture registry:

```c
struct obmm_gsva_aperture {
    uint64_t generation;
    uint64_t base;
    uint64_t size;
    uint64_t flags;
    uint32_t owner_node_count;
    uint32_t state;
};
```

OBMM uses it to:

1. Validate `OBMM_CMD_EXPORT` requests that want GSVA backing.
2. Validate `OBMM_CMD_IMPORT` requests that map a GSVA segment.
3. Reject normal `obmm_shmdev` mmap into GSVA space unless the mapping is tied
   to a GSVA segment lease.
4. Report address ownership and conflicts through sysfs/debugfs.

## 8. Allocation Model

### 8.1 Segment Allocation

Applications request GSVA through a manager API:

```c
int gva_alloc_shared(const struct gva_alloc_req *req,
                     struct gva_alloc_resp *resp);
```

Suggested request:

```c
struct gva_alloc_req {
    uint64_t generation;
    uint64_t size;
    uint64_t alignment;
    uint32_t home_node_id;
    uint32_t access_flags;
    uint32_t cache_policy;
    uint32_t flags;
};
```

Suggested response:

```c
struct gva_alloc_resp {
    uint64_t segment_id;
    uint64_t gsva_base;
    uint64_t size;
    uint32_t home_node_id;
    uint32_t token_id;
};
```

The allocator can be centralized by convention or distributed. For first
implementation, use deterministic owner-sharded allocation:

```text
node_slice_base = aperture_base + home_node_id * node_stride
segment allocated from home node's slice
```

This avoids distributed free-list contention and keeps failure diagnosis simple.

### 8.2 Allocation Announcements

The home manager announces every segment:

```c
struct gva_mgr_segment_announce {
    uint64_t generation;
    uint64_t segment_id;
    uint64_t gsva_base;
    uint64_t size;
    uint32_t home_node_id;
    uint32_t home_cna;
    uint32_t token_id;
    uint32_t access_flags;
    uint32_t cache_policy;
    uint32_t state;
};
```

Peers must acknowledge before the segment becomes globally active. The home
manager may allow local-only preparation before peer ack, but applications
should not publish pointers until the segment is active.

### 8.3 Free and Reuse

Free is generation-fenced:

```text
retire segment -> peer unmap ack -> home unexport -> allocator releases range
```

A `gsva_base` must not be reused until every manager has acknowledged the retire
message for the current generation. This prevents stale pointer descriptors from
silently naming a new object.

## 9. Mapping Interfaces

GSVA needs a user-visible mapping interface that says “allocate/map from the
reserved GSVA aperture”.

Two acceptable API shapes:

### 9.1 Explicit GVA Manager API

```c
void *gva_mmap_shared(uint64_t segment_id,
                      uint64_t offset,
                      size_t length,
                      int prot,
                      uint32_t flags);
```

The manager resolves `segment_id -> gsva_base`, validates that the target range
is inside the reserved aperture, opens the correct `obmm_shmdev`, and performs
fixed-address mmap at:

```text
gsva_base + offset
```

### 9.2 mmap Flag

Add a GSVA-specific mmap path:

```text
mmap(NULL, length, prot,
     MAP_SHARED | MAP_GSVA,
     obmm_shmdev_fd, offset)
```

or:

```text
mmap(gsva_base, length, prot,
     MAP_SHARED | MAP_FIXED | MAP_GSVA,
     obmm_shmdev_fd, offset)
```

The kernel/OBMM handler interprets `MAP_GSVA` as:

```text
1. lookup segment lease for fd + offset
2. compute required GSVA address
3. reject if caller's requested VA differs
4. install mapping only inside reserved GSVA aperture
```

For first implementation, the explicit manager API is lower risk. It avoids
changing generic mmap flag plumbing before the control plane is stable.

## 10. OBMM Export and Import Semantics

### 10.1 Home Export

Home manager flow:

```text
gva_alloc_shared(home_node=this_node)
  -> reserve segment in distributed manager state
  -> OBMM_CMD_EXPORT with GSVA flag/private metadata
  -> OBMM records mem_id as backing segment_id
  -> home mmap must be installed at gsva_base
  -> GVA map programmed with uba_base=gsva_base, pte_offset=0
```

OBMM must not return a different architectural UBA for GSVA export. If the
underlying UMMU path needs internal physical addresses, those are not exposed as
the GSVA UBA.

Recommended UAPI extension:

```c
struct obmm_gsva_export_priv_v1 {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t generation;
    uint64_t segment_id;
    uint64_t gsva_base;
    uint64_t size;
    uint32_t flags;
};
```

For a GSVA export:

```text
cmd_export.uba == gsva_base
```

If OBMM cannot honor that, the export must fail. It must not return a different
UBA and ask QEMU to paper over the mismatch.

### 10.2 Peer Import

Peer manager flow:

```text
receive segment announcement
  -> OBMM_CMD_IMPORT with GSVA private metadata
  -> imported local PA window allocated as today
  -> OBMM/GVA callback programs route:
       user_va=gsva_base
       uba_base=gsva_base
       pte_offset=0
       home={home_cna, token_id}
  -> peer mmap fixed at gsva_base
```

Recommended import private payload:

```c
struct obmm_gsva_import_priv_v1 {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t generation;
    uint64_t segment_id;
    uint64_t gsva_base;
    uint64_t size;
    uint32_t token_value;
    uint32_t flags;
};
```

For a GSVA import:

```text
remote_uba == gsva_base
```

The peer's local imported PA window remains an implementation detail used to
make current QEMU memory regions work. It is not the application-visible
address and not the architectural UBA.

## 11. GVA Control Plane Integration

GSVA should reuse the explicit GVA model:

```text
GVA Map:
  local_va   = gsva_base
  home_va    = gsva_base
  local_pa   = imported PA window or home backing PA
  pte_offset = 0
  uba_base   = gsva_base
  vmid/asid  = manager-selected context
  dcna/tid   = home route
  map_source = gva_manager
  profile    = gsva_identity
```

MMU.S3 route key:

```text
{VMID, ASID, UBA=gsva_base+offset}
```

Route result:

```text
{home_cna, TID, UPI, p_tag}
```

QEMU must not see GSVA as a private remapping layer. It should see a GVA route
whose UBA is the same value the application uses as a pointer.

## 12. Manager Protocol over OBMM MPMC

### 12.1 Message Types

Minimum message set:

```c
enum gva_mgr_msg_type {
    GVA_MGR_MSG_HELLO = 1,
    GVA_MGR_MSG_APERTURE_PROPOSE = 2,
    GVA_MGR_MSG_APERTURE_ACCEPT = 3,
    GVA_MGR_MSG_APERTURE_COMMIT = 4,
    GVA_MGR_MSG_SEGMENT_ANNOUNCE = 5,
    GVA_MGR_MSG_SEGMENT_ACK = 6,
    GVA_MGR_MSG_SEGMENT_RETIRE = 7,
    GVA_MGR_MSG_SEGMENT_RETIRED_ACK = 8,
    GVA_MGR_MSG_HEARTBEAT = 9,
    GVA_MGR_MSG_ERROR = 10,
};
```

Common header:

```c
struct gva_mgr_msg_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint64_t generation;
    uint64_t seq;
    uint32_t src_node;
    uint32_t dst_node;
    uint32_t payload_len;
    uint32_t crc32;
};
```

### 12.2 Queue Layout

Use existing OBMM MPMC queue design:

```text
manager_control_region[node]
  -> rx lanes from every peer
  -> tx descriptors for every peer
  -> manager state snapshot
```

The first version can implement logical MPMC as one SPSC lane per source and
destination, because that matches the current validated owner-sharded queue
model and avoids remote atomics.

### 12.3 Ordering

Manager messages need release/acquire semantics:

1. Producer writes payload.
2. Producer release-stores descriptor.
3. Consumer acquire-loads descriptor.
4. Consumer validates generation and checksum.

No cross-node compare-and-swap is required in Phase A.

## 13. Guest Kernel and OBMM Responsibilities

### 13.1 Guest Kernel

The guest kernel needs a GSVA aperture registry that can:

1. Reserve a process VA range for GSVA.
2. Reject ordinary `mmap` collisions with that range where practical.
3. Validate GSVA mmap requests against active segment leases.
4. Expose diagnostic state:
   - active aperture
   - active segments
   - per-process reservations
   - conflict counters

The first implementation may be scoped to GSVA-aware test processes instead of
all processes, but the design target is kernel-visible reservation.

### 13.2 OBMM

OBMM needs to:

1. Accept GSVA export/import metadata.
2. Ensure `cmd_export.uba == gsva_base` for GSVA exports.
3. Ensure import `remote_uba == gsva_base`.
4. Reject non-GSVA shmdev mappings into the GSVA aperture.
5. Invoke GVA map/unmap callbacks with `local_va=gsva_base`,
   `uba_base=gsva_base`, and `pte_offset=0`.
6. Preserve existing non-GSVA OBMM behavior.

## 14. Failure Semantics

GSVA must fail explicitly:

1. No common aperture:
   - bootstrap generation fails.
2. Local kernel cannot reserve aperture:
   - manager broadcasts error and generation fails.
3. OBMM cannot honor `uba == gsva_base`:
   - export/import fails.
4. A process maps ordinary memory into GSVA aperture before reservation:
   - manager process setup fails with address conflict.
5. A GSVA mmap request asks for a different VA:
   - kernel/OBMM rejects with `EINVAL`.
6. Segment overlap:
   - manager rejects allocation before OBMM is called.
7. Peer does not ack segment:
   - segment remains pending and cannot be published to applications.

There is no fallback to `pte_offset != 0` in GSVA mode. That fallback belongs
to GVA relocation mode, not GSVA.

## 15. Cache and Consistency

GSVA address equality does not imply hardware cache coherence.

First implementation policy:

1. Home local export mapping may remain cacheable.
2. Remote imports default to `O_SYNC` / non-cacheable OBMM mappings.
3. Manager queues use the existing single-writer queue layout.
4. Shared descriptors use release/acquire ordering.
5. Cacheable remote GSVA is out of scope until ownership/coherence simulation is
   added.

This keeps the programming model simple without overstating consistency.

## 16. Implementation Phases

### Phase A: Manager Bootstrap and Aperture Reservation

Scope:

1. Add `gva_manager` userspace component for the W4 guest.
2. Build manager MPMC queues over existing OBMM shared-memory pool.
3. Implement aperture proposal/accept/commit protocol.
4. Add kernel/OBMM GSVA aperture registry.
5. Add per-process aperture reservation helper.
6. Expose manager and kernel diagnostic dumps.

Acceptance:

1. Dual-node managers agree on the same aperture.
2. Four-node managers agree on the same aperture.
3. Artificial address conflict causes deterministic bootstrap failure.
4. Existing OBMM queue demos still pass without GSVA mode.

### Phase B: GSVA Segment Allocation and Mapping

Scope:

1. Add `gva_alloc_shared()` and `gva_mmap_shared()`.
2. Add owner-sharded segment allocator inside the aperture.
3. Extend OBMM export/import private metadata for GSVA.
4. Ensure GSVA export returns `uba == gsva_base`.
5. Ensure home and peers fixed-map shmdev at `gsva_base`.
6. Program GVA maps with `pte_offset=0`.

Acceptance:

1. Dual-node demo proves pointer equality:
   - home pointer value equals peer pointer value.
   - home and peer read/write through the same numeric pointer.
2. QEMU/GVA logs show `user_va == uba == home_va`.
3. Nonzero `pte_offset` in GSVA path is rejected.

### Phase C: Full-Mesh GSVA

Scope:

1. Extend manager bootstrap to eight nodes.
2. Map every node's home slice into every process.
3. Add segment retire and generation-fenced reuse.
4. Add pointer descriptor exchange workload.

Acceptance:

1. Four-node and eight-node runs can exchange pointer descriptors.
2. Every node can dereference every other node's GSVA segment at the same
   numeric address.
3. Duplicate or overlapping allocation proposals are rejected.

### Phase D: mmap Flag Path

Scope:

1. Add kernel-supported GSVA mmap flag or equivalent OBMM shmdev mmap mode.
2. Allow applications to request GSVA mappings without going through a helper
   wrapper.
3. Keep explicit manager API as the stable orchestration layer.

Acceptance:

1. `MAP_GSVA` or equivalent rejects mappings outside active GSVA segments.
2. Existing non-GSVA mmap behavior remains unchanged.

## 17. Test Plan

Required command-line entries:

```text
gva_manager --bootstrap --node-id N --node-count C
obmm_gsva_demo --mode identity
obmm_gsva_demo --mode conflict
obmm_gsva_demo --mode stale-generation
```

Harness entries:

```text
run_ub_dual_node_gsva_manager_bootstrap.sh
run_ub_dual_node_gsva_demo.sh
run_ub_four_node_gsva_demo.sh
run_ub_eight_node_gsva_demo.sh
```

Data assertions:

```text
home writes *(uint64_t *)gsva_ptr = A
peer reads the same pointer value and sees A
peer writes *(uint64_t *)gsva_ptr = B
home reads the same pointer value and sees B
```

Management assertions:

```text
all managers report same aperture base/size/generation
kernel reports GSVA aperture reserved
OBMM reports GSVA segment active
GVA route dump reports pte_offset=0 and uba_base=gsva_base
```

Regression:

```text
cargo test --workspace
run_ub_dual_node_demo.sh
run_ub_dual_node_obmm_import_stress.sh
run_ub_four_node_obmm_queue_demo.sh
```

## 18. User Impact

Without GSVA, shared descriptors need translation:

```text
descriptor = {node_id, region_id, offset}
receiver resolves descriptor to local VA
```

With GSVA:

```text
descriptor = {ptr}
receiver dereferences ptr directly
```

The value of the design is not just shorter descriptors. It moves the simulator
toward the real global virtual address programming model: the OS, OBMM, GVA
manager, and QEMU S3/NoC model agree on one address value.

## 19. Open Questions

1. Should the first kernel-visible aperture registry live under OBMM or a new
   GVA device?
2. What GSVA aperture base is valid for every current W4 guest process layout?
3. Should the first manager be a userspace daemon, a kernel module, or a hybrid?
4. How strict should kernel protection be for non-GSVA processes that collide
   with the GSVA aperture?
5. How should `cmd_export.uba == gsva_base` be implemented inside the current
   OBMM/UMMU path without breaking non-GSVA exports?
