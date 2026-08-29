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

Input publication is ordered by the release fence in `seed_region()` before
the bootstrap record is published. The workload also attempts `msync()` when
the mapping provider supports it. Linux device/PFN mappings can return
`EINVAL` because they have no filesystem writeback operation; the workload
records `msync_unsupported=1` and continues in that case. Any other `msync()`
error remains fatal.

The consumer uses the import aperture address returned by
`obmm_alloc_import_pas()` as the memref's simulator UB GM address. That value
is also passed to the OBMM import operation and QEMU mapping table. Device/PFN
VMAs do not expose a usable PFN through `/proc/self/pagemap`, so the workload
does not attempt to reconstruct the import aperture from its userspace virtual
mapping. Ordinary anonymous metadata pages still use pagemap translation for
the dispatch DMA address.

Example guest invocations:

```text
lingqu_shmem_pto_direct --role producer --node-id 0 --node-count 2 \
  --elements 16384 --generation 101 --timeout-ms 120000

lingqu_shmem_pto_direct --role consumer --node-id 1 --node-count 2 \
  --elements 16384 --generation 101 --timeout-ms 120000 \
  --requester-cna 0xf002 --artifact-fingerprint 0x1234
```

The default `--expect success` mode exits successfully only after the producer
observes the transformed output and the consumer receives a successful PTO
completion. `--expect authorization-timeout` is an explicit negative-test
mode. In that mode the producer requires the complete output tensor to retain
the `0x7fc00001` sentinel, while the consumer requires completion status 3 and
the exact `pto_ub_gm_authorization_timeout` code. When those conditions hold,
both roles report the expected failure as a passing test outcome and return
zero, so the PID 1 guest launcher stays healthy. Any changed output, successful
dispatch, or different completion error remains a test failure.

Callable 1 currently identifies the frozen host-vector artifact whose PTO
kernel executes one `128 x 128` `f32` tile. For each element it computes
`c = a + b`, followed by `f = (c + 1) * (c + 2)`. The producer verifies this
callable-specific result in its original export mapping. The workload rejects
any other element count before exporting or importing memory, so a shorter
memref cannot reach the kernel and fail during its fixed-size `TLOAD`.
