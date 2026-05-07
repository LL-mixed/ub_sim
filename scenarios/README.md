# Simulator Scenarios

Scenario YAML files define the Rust simulator and Linqu UAPI bridge logical
topology. QEMU/FM link topology is configured separately with
`vendor/ub_topology_*.ini`.

Current scenarios:

- `mvp_2host_single_domain.yaml`: dual-node default.
- `mvp_4host_single_domain.yaml`: four guest nodes, one logical UBPU per host.
- `mvp_8host_single_domain.yaml`: eight guest nodes, one logical UBPU per host.

When a QEMU harness launches multiple guest nodes, its
`SIM_UAPI_SCENARIO_CONFIG` default must match the harness node count. The
`TOPOLOGY_FILE` default controls the QEMU/FM full-mesh links; the scenario YAML
controls the Rust UAPI/chipbackend view used inside that QEMU process.
