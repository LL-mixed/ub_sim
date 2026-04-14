# Eight-Node Full-Mesh Design Notes

## Goals

Scale the current simulator from a validated four-node full mesh to an
eight-node full mesh without reintroducing local identity hacks.

## Hard Constraints

### 1. FM owns identity

In clustered mode:

- `EID` is FM-managed
- `CNA` is FM-managed

Implications:

- demos may read `EID/CNA` from guest-visible FM state
- guest kernel may consume `EID/CNA` from FM state
- QEMU send/data paths may only route using FM-published identity and route
  state
- no local fallback synthesis of `EID`
- no local fallback synthesis of `CNA`

### 2. Eight-node full mesh requires 7 ports per node

Current four-node implementation works because:

- 4 nodes
- each node has 3 peers
- current QEMU `port_num=3`

Eight-node full mesh requires:

- 8 nodes
- each node has 7 peers
- therefore `port_num >= 7`

## Current Gaps

1. QEMU `virt.c` still hardcodes:

```c
ubc_dev_state->parent.port.port_num = 3;
```

2. Four-node launch/matrix scripts are specialized and do not yet generalize to
   eight-node orchestration.

3. Topology samples stop at four nodes.

## First Implementation Step

### QEMU port count must be configurable

Immediate change:

- replace the `port_num=3` hardcode with a configurable value
- keep default `3` to avoid breaking current four-node behavior

Recommended source:

- environment variable, for example `UB_SIM_PORT_NUM`

Required behavior:

- default to `3`
- reject `0`
- reject values above `UB_DEV_MAX_NUM_OF_PORT`

## Second Implementation Step

### Add an eight-node full-mesh topology sample

The topology should declare:

- `nodeA` ... `nodeH`
- 28 links
- each node uses local ports `0..6`

## Third Implementation Step

### Multi-node launcher and matrix harness

Do not use tmux as the automation control plane.

Required split:

- `tmux` launcher remains interactive/debug only
- matrix harness remains headless

## Acceptance Gate For Eight-Node Bring-Up

Before any eight-node demo matrix is trusted, the environment must show:

1. each node exposes 7 active ports
2. FM assigns unique `EID` per node
3. FM assigns unique primary `CNA` per node
4. route table converges without fallback routing
