# Lingqu shmem PTO direct-access workload

This directory defines the guest-side two-node acceptance workload for PTO
`AddressSpace::UB_GM` access to `lingqu_shmem`. The source, runner, and runtime
evidence are delivered and validated as part of P3.

- The producer exports one OBMM region, writes two contiguous `f32` inputs,
  initializes the output, publishes bootstrap metadata, and verifies the
  output from the original export mapping.
- The consumer imports that exact region, registers its endpoint map, creates
  three opaque `lingqu_shmem_memref` objects, submits the existing
  `IoOpcode::Dispatch` tag-10 slot, and releases the mapping after completion.

The artifact fingerprint and PTO requester CNA are required command-line
inputs. The host runner derives them from the selected artifact manifest and
QEMU device configuration; the guest does not guess either identity.

Example guest invocations:

```text
lingqu_shmem_pto_direct --role producer --node-id 0 --node-count 2 \
  --elements 1024 --generation 101 --timeout-ms 120000

lingqu_shmem_pto_direct --role consumer --node-id 1 --node-count 2 \
  --elements 1024 --generation 101 --timeout-ms 120000 \
  --requester-cna 0xf002 --artifact-fingerprint 0x1234
```
