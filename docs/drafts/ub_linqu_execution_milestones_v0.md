# UB/Linqu Execution Checklist and Milestones (v0)

This document turns the current mainline plan into concrete execution milestones.

It is aligned with:
- `draft/ub_linqu_master_and_branch_plan_v0.md`
- `draft/qemu_8_2_0_ub_single_vs_multi_node_contract_v0.md`

---

## Guiding Rule

Execution order must remain:

1. Goal 1 constrains Goal 2
2. Goal 2 builds the platform basis for Goal 4
3. Goal 4 serves Goal 3

That means:
- do not build private shortcuts that break `UB/Linqu` platform conformance
- do not move workload/service integration ahead of multi-node UB topology

---

## Milestone M0: Stable Single-Node Native UB Bring-up

Status:
- mostly achieved

Scope:
- `simulator/vendor/qemu_8.2.0_ub`
- real ARM64 guest
- real Linux `drivers/ub`

Checklist:
- `UBIOS` discovery works
- `UBC` probe/init works
- `UMMU` probe/init works
- `ubfi/ubus/hisi_ubus` bring-up works
- `ub-hotplug` binds
- `slot0/power = on`
- downstream `00002` enumerates
- `decoder create success`
- `RESOURCE 0 assigned`
- downstream `port0/linkup = 1`

Exit criteria:
- guest boots repeatedly without regressing these points
- no known early bring-up blocker remains in the current single-node path

---

## Milestone M1: Multi-Port UBC Base

Status:
- in progress
- significant parts already achieved

Current state:
- primary `UBC` is now 2-port
- `port0` is connected
- `port1` is guest-visible and unconnected

Checklist:
- `UBC.total_num_of_port = 2` is visible to guest
- `port1` appears in guest sysfs
- `port1/linkup = 0`
- `port1/neighbor = No Neighbor`
- `slot0` still only owns `port0`
- route-table sizing follows actual `port_num`
- existing `port0 -> 00002` path keeps working

Exit criteria:
- `port0` path remains green
- `port1` is stable and reserved for future inter-node use
- no accidental coupling between `slot0` and `port1`

---

## Milestone M2: Minimal Two-Node UBC<->UBC Contract

Status:
- not started

Objective:
- define the smallest cross-instance contract before implementing transport

Target topology:
- `nodeA.ubc.port1 <-> nodeB.ubc.port1`

Link model:
- one explicit endpoint per side
- topology ownership sits in `ub_fm`
- one standalone bidirectional `UBLink`-style interconnect object between them
- one link instance per point-to-point connection
- if multiple port pairs are connected, compose the topology from multiple link instances
- no direct host-local object reach-through as the final model
- control-plane traffic must cross the link explicitly
- `ub_fm` manages topology and link instantiation
- `UBLink` manages per-link runtime behavior

Checklist:
- define per-node GUID/EID ownership rules
- define symmetric `neighbor_guid` semantics
- define symmetric `neighbor_port_idx` semantics
- define symmetric `linkup` semantics
- define the static topology configuration format used at initialization:
  - nodes
  - roles
  - port-pair links
  - initial link state
- define the initial file-backed topology snapshot shape:
  - INI-style format
  - one `[link "..."]` section per point-to-point connection
  - keys:
    - `a_device_id`
    - `a_port_idx`
    - `b_device_id`
    - `b_port_idx`
    - `link_up`
- define pending-link semantics for unresolved remote endpoint IDs:
  - static config may name remote endpoints not present in the local process
  - such links stay pending in `ub_fm/UBLink`
  - such links must not break single-node boot or local topology apply
- define the later runtime dynamic topology control path
- define how route visibility should behave across nodes
- define what parts of msgq / neighbor / topology state must cross instance boundaries
- define the minimum message classes that must cross the link first:
  - discovery / enum messages
  - config read/write messages
  - route/topology queries
- define the link-instance lifecycle:
  - creation
  - endpoint attachment
  - symmetric link-up/down state
  - teardown or reconfiguration semantics
- keep the implementation model strict:
  - one `UBLink` instance per point-to-point connection
  - multiple interconnected port pairs require multiple `UBLink` instances
- define the `ub_fm` responsibilities explicitly:
  - topology declaration
  - link creation
  - endpoint attachment
  - state propagation back into guest-visible `UBC` port config
- define the topology source contract explicitly:
  - configuration file for static initialization
  - explicit runtime control path for dynamic updates
  - both must reuse the same internal reconciliation path
- define the `UBLink` responsibilities explicitly:
  - point-to-point link state machine
  - endpoint attach/detach
  - per-link control-plane forwarding

Deliverable:
- markdown contract doc for 2-node direct interconnect
- draft topology configuration schema for static initialization
- single-node and two-node example topology files
- outline of runtime dynamic-topology control surface

Exit criteria:
- no ambiguous ownership for controller, port, GUID, or route state
- enough detail exists to implement the first direct link

---

## Milestone M3: First Two-Node Direct Link

Status:
- not started

Objective:
- implement the first real interconnect between two QEMU instances

Checklist:
- create two QEMU instances with one primary `UBC` each
- connect `nodeA.port1` to `nodeB.port1`
- instantiate the link from static configuration, not hardcoded machine-local wiring
- expose symmetric link state in guest
- expose symmetric neighbor metadata in guest
- keep `port0` local path intact on each node
- ensure guest-visible state is not faked in only one direction
- make `ub_fm` the owner of the connection lifecycle and state propagation
- forward the first minimal control-plane traffic across the link
- verify that local `probe/init` completion is followed by real cross-node communication over `port1`

Validation:
- both guests boot
- both guests see the peer link
- both guests report stable neighbor and link state
- both guests can complete at least the first minimal control-plane exchanges over the interconnect

Exit criteria:
- first cross-instance `UBC <-> UBC` link exists and is visible to real `drivers/ub`
- the link is not topology-only; it already carries real control-plane communication
- the first cross-instance link is bootstrapped from configuration files

---

## Milestone M4: Multi-Node Resource and Route Semantics

Status:
- not started

Objective:
- move from topology-only interconnect to usable multi-node platform semantics

Checklist:
- verify route state across two nodes
- verify resource/decoder behavior does not remain host-local
- verify guest-visible resource assignment remains conformant across nodes
- identify what remote entity/resource surfaces must exist

Exit criteria:
- two-node system is not just linked, but begins to behave like a real UB platform

---

## Milestone M5: Service Placement on Native UB Topology

Status:
- not started on mainline

Objective:
- place service roles onto the UB-interconnected multi-node system

Checklist:
- decide node-role schema
- define service placement config
- map service roles such as:
  - `block`
  - `shmem`
  - `dfs`
  - `db`
- ensure placement matches guest-visible topology, not a host-side shortcut model

Exit criteria:
- services are placed onto actual UB-interconnected nodes
- the placement model is configurable

---

## Milestone M6: rust_llm_server_mvp Validation

Status:
- not started on native UB mainline

Objective:
- run and validate `rust_llm_server_mvp` on top of the multi-node UB platform

Checklist:
- define target node topology for MVP
- define service placement required by MVP
- bring MVP up on the simulated system
- capture metrics and behavioral observations tied to the platform design

Exit criteria:
- MVP runs on the simulated multi-node UB system
- its key design choices can be tested against real platform topology and service placement

---

## Branch Checklist

These are support tracks, not the main execution path.

## Branch A: self-built linqu-ub

Use for:
- quick contract probing
- isolated MMIO/FDT/IRQ experiments

Do not use for:
- final proof of Goal 1
- final proof of Goal 2

Keep healthy:
- basic buildability
- minimal guest probe utility

## Branch B: host-side service simulator

Use for:
- fast service semantics iteration
- early `block/shmem/dfs/db` experiments

Do not use for:
- claiming native multi-node UB conformance

Keep healthy:
- service semantics
- backpressure/failure experiments
- mapping ideas for future native-node placement

## Branch C: workload harness

Use for:
- preserving workload-side validation logic
- later migration onto the native UB topology

Do not use for:
- front-running the platform work

Keep healthy:
- scenario definitions
- metrics/report shape
- placement assumptions that can later move to the native topology

---

## Immediate Next Tasks

Ordered next actions:

1. Finalize the 2-port `UBC` basis and keep it stable
2. Write the concrete 2-node `UBC.port1 <-> UBC.port1` contract
3. Implement the first direct interconnect
4. Validate it with real Linux `drivers/ub`
5. Only then begin native-node service placement work
