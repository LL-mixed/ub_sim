# UB Topology File Format (v0)

This note records the first static topology file format used by:

- `ub_fm`
- `UBLink`
- `UB_FM_TOPOLOGY_FILE`

The format is intentionally narrow. It is only meant to bootstrap:

- single-node snapshots
- first two-node `UBC <-> UBC` direct-link snapshots

## Shape

The file is INI-style.

Each point-to-point link is one section:

```ini
[link "name"]
a_device_id=...
a_port_idx=...
b_device_id=...
b_port_idx=...
link_up=true
```

## Required keys

- `a_device_id`
- `a_port_idx`
- `b_device_id`
- `b_port_idx`
- `link_up`

## Local node scoping

The first multi-instance form uses scoped endpoint IDs:

- `nodeA.ubcdev0`
- `nodeA.ubsw0`
- `nodeB.ubcdev0`

The current local-process selector is:

- environment variable: `UB_FM_NODE_ID`

Current behavior:

- if `UB_FM_NODE_ID=nodeA`, endpoint IDs prefixed with `nodeA.` are resolved to
  local device IDs by stripping the prefix
- endpoint IDs prefixed with other node IDs remain remote names
- links that do not involve the local node at all are ignored by the local
  process during snapshot installation

This allows one shared two-node topology file to be consumed by different QEMU
instances from different local-node viewpoints.

## Semantics

- one section represents one point-to-point link
- multiple port-pair interconnects require multiple sections
- `a` and `b` are symmetric; link identity is the unordered pair
- `link_up=true` means the link should be active once both endpoints resolve
- `link_up=false` means the link remains declared but inactive

## Pending endpoints

Endpoint IDs are allowed to be unresolved in the current process.

When that happens:

- the link remains declared
- `ub_fm` keeps it in topology state
- `UBLink` treats it as `pending`
- local boot and local topology apply must continue

This is required for:

- future-facing remote links in a single-node process
- later multi-process topology injection

## Invalid topology

The following are treated as invalid configuration and must be rejected:

- missing endpoint IDs
- an endpoint connected to itself
- duplicate links with conflicting state
- one endpoint reused by multiple different peers
- a local endpoint whose `port_idx` exceeds the locally realized `port_num`

Invalid configuration must not partially mutate the current active topology.

The intended flow is:

1. validate new snapshot
2. if validation fails, reject it
3. keep the current active topology unchanged

This is different from a `pending` link:

- `pending` means the declaration is valid but the remote endpoint is not yet resolvable
- `invalid` means the declaration is contradictory or impossible and must be rejected

## Current examples

- `simulator/vendor/ub_topology_single_node.ini`
- `simulator/vendor/ub_topology_two_node_v0.ini`

## What this format does not cover yet

Not covered in `v0`:

- explicit node-role sections
- route programming
- dynamic reconfiguration
- transport/channel selection for cross-process links
- service placement

Those will be layered on later without changing the core rule:

- static file input and dynamic runtime input must both converge into
  `topology source -> ub_fm -> UBLink -> UBC endpoint`
