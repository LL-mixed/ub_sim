# Four-Node Full-Mesh Matrix Validation

## Scope

This report captures the current four-node simulator state in the main repo.
The validated topology is a single `nodeA/nodeB/nodeC/nodeD` full mesh where
each node has three direct UB links to the other three nodes.

Validated demo families:

- `chat`
- `rpc`
- `rdma`
- `obmm-pool`

## Component Revisions

- `vendor/qemu_8.2.0_ub`
  - `a793c7a3f5` `ubc: fix four-node full-mesh chat routing and link roles`
  - `8502960958` `ubc: fix four-node rdma identity and rx completions`
  - `6afc68f9f4` `ubc: make sim decoder pool reads honor mapped dcna`
- `guest-linux/kernel_ub`
  - `c750da90172c` `ipourma: reschedule rx work by budget instead of draining`
  - `1591eab00e42` `ubcore: fix initiator bind_jetty compat tp state`
- `simulator`
  - `5e829cb` `simulator: add four-node full-mesh tmux launcher`
  - `6a0b926` `simulator: add four-node smoke harness`
  - `a022cc0` `simulator: add headless four-node chat matrix`
  - `1983ac5` `simulator: make four-node tmux wait for guest bootstrap`
  - `e8d62b6` `simulator: add four-node rpc matrix and clean demo roles`
  - `061a29e` `simulator: add four-node rdma matrix`
  - `ae7d86f` `simulator: replace obmm demo with pool validation`

## Topology

Topology file:

- [ub_topology_four_node_full_mesh.ini](/Volumes/repos/pypto_workspace/ub_sim.git/vendor/ub_topology_four_node_full_mesh.ini)

Properties:

- 4 nodes
- 6 direct links
- 3 active ports per node
- no relay links

## Validation Runs

### Four-Node Smoke

Validated main-repo run id:

- `2026-04-14_10-00-35_smoke4_3634_iter1_tmux4`

Validated:

- all four guests reach shell after `/bin/run_demo`
- FM links are ready
- route table converges with `entry_num=7`
- each node reports `ports=3`

### Chat Matrix

Validated main-repo run id:

- `2026-04-14_19-58-32_chat4_27617_headless4`

Result:

- all 6 undirected pairs passed

### RPC Matrix

Validated main-repo run id:

- `2026-04-14_19-59-41_rpc4_27222_headless4`

Result:

- all 12 directed calls passed
- every node acted as both server and client

### RDMA Matrix

Validated main-repo run id:

- `2026-04-14_20-27-17_rdma4_1513_headless4`

Result:

- all 12 directed request/reply calls passed

Important constraints proven by this run:

- per-node EIDs are unique
- route-by-EID no longer relies on fallback guesses
- multicast control traffic uses explicit fanout

### OBMM Pool

Validated main-repo run id:

- `2026-04-14_22-23-24_obmmpool4_28163_headless4`

Result:

- pass

Current semantics:

- each node exports one region
- each node imports all remote regions
- the shared pool advances by round
- owner writes payload to its local slot
- non-owners verify that owner payload through imported views
- non-owners ACK by writing to their own local slots
- owner waits for all ACKs, then emits `ROUND_COMMIT`

This is no longer pairwise `export/import` smoke. It is a multi-node shared
buffer-pool visibility check.

## Current Conclusions

1. Four-node full-mesh bring-up is stable enough to support repeated headless
   demo matrices.
2. `chat`, `rpc`, and `rdma` already have four-node matrix coverage.
3. `obmm` mainline validation has moved to the pool demo rather than the old
   pairwise demo.
4. For clustered runs, identity must remain FM-owned:
   - `EID` must be unique per node and assigned/published by FM.
   - `CNA` must be assigned/published by FM.
   - demo code must only consume those values, never synthesize them locally.
