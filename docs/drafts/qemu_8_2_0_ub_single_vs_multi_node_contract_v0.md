# qemu_8.2.0_ub: single-node status and minimal multi-node UB contract (v1)

## Current single-node status

Validated on macOS-built `qemu-system-aarch64` from `simulator/vendor/qemu_8.2.0_ub` with real Linux `drivers/ub`:

- `ubfi` discovers `UBIOS` and creates the primary `UBC`.
- `ummu` probes and registers successfully.
- `hisi_ubus` loads and binds.
- `ub-hotplug` binds as `00001:service002`.
- `slot0` is created and reports `power = on`.
- The primary `UBC` is now guest-visible as a 2-port controller:
  - `port0` remains the currently wired hotplug/downstream path
  - `port1` is visible in guest and intentionally left unconnected
- A downstream UB device is enumerated as `00002`.
- `00002` is currently modeled as a switch-like UB device:
  - `class_code = 0x0003`
  - `type = 0x3`
  - `primary_entity = 0x002`
- `00001/port1` now reports:
  - `boundary = 0x0`
  - `linkup = 0`
  - `neighbor = No Neighbor`
- `00002/port0` now reports:
  - `boundary = 0x0`
  - `linkup = 1`
  - `neighbor = 00001`
  - `neighbor_guid = cc08-0541-...-0001`
  - `neighbor_port_idx = 0`
- `00002` now has a real downstream resource assignment:
  - `/sys/bus/ub/devices/00002/resource0`
  - `/sys/bus/ub/devices/00002/resource0_wc`
  - kernel logs show `decoder create success` and `RESOURCE 0: assigned`

## Current single-node gaps

The next meaningful gap is no longer hotplug discovery, nor the first
`resource/decoder` step. The next single-node gap is:

- `00002` still does not register its own `ub_service` device
- the current downstream switch path is good enough as a bring-up sample, but
  it is not the preferred foundation for multi-node interconnect
- the more important contract now is multi-port `UBC`, because that is the
  cleaner basis for later `UBC <-> UBC` direct links across QEMU instances

## Why this matters for target #1 and #2

Target #2 must remain constrained by target #1.

That means:
- It is not enough for a single QEMU instance to expose locally plausible UB behavior.
- The modeled controller/entity/slot/port/resource relationships must remain conformant when the simulator grows into multiple UB-interconnected systems.

So the next work must preserve two invariants:
- single-node guest-visible UB contract must remain spec-conformant enough for real `drivers/ub`
- the same contract must scale to multi-instance interconnect without inventing a different private model

## Minimal multi-node contract (2-system thought model)

Model two QEMU instances, `nodeA` and `nodeB`.

Each node contains:
- one primary `UBC`
- one `UMMU`
- one local root entity (`entity_idx = 0`)
- at least two ports

Minimal interconnect contract:
- `nodeA.ubc.port1` is linked to `nodeB.ubc.port1`
- `port0` can continue to carry local single-node bring-up topology during the
  transition period
- each side exposes the other as a remote UB entity/device via normal UB discovery
- neighbor relation must be symmetric:
  - `A.port1.neighbor_guid == B.primary.guid`
  - `B.port1.neighbor_guid == A.primary.guid`
- link state must be symmetric and stable
- route/resource visibility must not depend on host-local shortcuts
- message/queue transport must preserve UB message semantics across instances

## Implications for implementation

The current single-instance switch child is acceptable as a stepping stone, but
not as the final multi-node model.

The next implementation layers should be:
1. Single-node multi-port `UBC` contract
- keep `port0` as the already-working downstream path
- keep `port1` guest-visible and reserved for future inter-node links
- make route-table sizing and port-visible config depend on real `port_num`

2. Inter-instance transport contract
- define how one QEMU instance forwards UB msgq / route / neighbor traffic to another
- keep GUID/EID/CNA ownership explicit per instance

3. Topology declaration
- define a config format for:
  - node list
  - per-node UBC/UMMU shape
  - cross-node port links
  - later service roles (`block/shmem/dfs/db`)

## Immediate next step

Before actual 2-node wiring, keep tightening the single-node multi-port basis:
- preserve the current `port0 -> 00002` path
- treat `port1` as the future `UBC <-> UBC` link point
- then define the smallest cross-instance transport that can make
  `port1/linkup`, `neighbor_guid`, and msgq semantics symmetric across nodes
