# Simulator Scenarios

Scenario YAML files define the Rust simulator and Linqu UAPI bridge logical
topology. QEMU/FM link topology is configured separately with
`vendor/ub_topology_*.ini`.

Current scenarios:

- `mvp_2host_single_domain.yaml`: dual-node default.
- `mvp_2host_p2b_remote_10ms.yaml`: deterministic 10 ms remote-load
  overlap used by the P2B two-node producer/consumer acceptance test.
- `mvp_4host_single_domain.yaml`: four guest nodes, one logical UBPU per host.
- `mvp_8host_single_domain.yaml`: eight guest nodes, one logical UBPU per host.

Every checked-in scenario carries explicit `remote_memory_model` and
`scheduler_core_model` sections. The latter describes P2B logical-coroutine,
pending-load, event-queue capacities, and the simulated clock. The Context
Store and scheduling policy are guest EL0 runtime state and therefore are not
QEMU model parameters. The section does not enable P2B unless the QEMU
launcher also selects scheduler-core mode.
The section is inert for commands that do not install the OBMM remote model
manifest, and is the single source of truth for OBMM latency/failure runs.
Experiment-specific changes belong in a new `mvp_<N>host_<purpose>.yaml` file;
do not override individual model fields with launcher environment variables.

When a QEMU harness launches multiple guest nodes, its
`SIM_UAPI_SCENARIO_CONFIG` default must match the harness node count. The
`TOPOLOGY_FILE` default controls the QEMU/FM full-mesh links; the scenario YAML
controls the Rust UAPI/chipbackend view used inside that QEMU process.
