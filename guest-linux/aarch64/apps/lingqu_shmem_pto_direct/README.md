# Lingqu shmem PTO direct-access workload

This directory defines the guest-side two-node acceptance workload for PTO
`AddressSpace::UB_GM` access to `lingqu_shmem`. The source, runner, and runtime
evidence cover the P3 happy path and the P4 lifecycle and fault gates.

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

lingqu_shmem_pto_direct --role consumer --node-id 1 --node-count 2 \
  --elements 16384 --generation 101 --timeout-ms 5000 \
  --cancel-after-ms 10 --expect authorization-cancelled \
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

`--expect authorization-cancelled` is the active-cancellation test mode. It
requires `--cancel-after-ms` to fall strictly inside the endpoint timeout.
After the dispatch enters pending authorization, the endpoint writes the
operation identity and then rings the cancel doorbell. It continues polling
until it receives exactly one status-3 completion carrying
`pto_ub_gm_authorization_cancelled`. The producer applies the same complete
sentinel scan used by the timeout test. The host gate additionally proves
that the matching command-queue head advanced once, no authorization or data
callback ran, and an optionally injected late completion was ignored after
the pending snapshot had been destroyed.

The QEMU-only duplicate-completion injection applies to delayed successful
authorization. Each legitimate timer completion resumes the command once;
the immediately repeated completion must be rejected by the active
operation/request/sequence guard with `reason=already_completed`. These
injection switches are host test controls and are absent from the guest
workload ABI.

The host runner's `--reset-on-pending 1` mode starts both guests under QMP,
waits for nodeB to log the first suspended authorization, and issues a strict
`system_reset` to nodeB. QEMU must discard the pending snapshot without a CQ
completion, reject an optional post-reset timer event with `reason=no_pending`,
and retain the monotonic authorization sequence. The rebooted consumer then
submits the workload again and must finish through the ordinary success path.
This makes reset cleanup, stale-event rejection, and post-reset recovery one
end-to-end gate; the guest workload ABI has no reset-specific option.

P4C adds explicit negative-test outcomes `bad-memref` and `access-denied`.
They must be paired with one test-only `--fault-case` value:

- `bad-mapping-ref` changes the generation encoded in the first opaque map
  reference while preserving a valid wire encoding;
- `stale-mapping` retires the registered endpoint map after materialization
  and before submission;
- `released-import` unmaps and unimports the consumer's OBMM import after
  materialization while preserving the registered endpoint-map reference;
- `retired-segment` uses a separate OBMM control export to wait until the
  producer unexports the payload segment, while preserving the consumer
  import, endpoint map, and prepared wire metadata.  The control export is
  one 2 MiB OBMM memseg; only its fixed-size header carries the handshake;
- `wrong-requester` replaces the validated requester CNA with a different,
  syntactically valid CNA;
- `oob` moves the output view one byte beyond the registered map;
- `address-overflow` makes the first UB GM range overflow `uint64_t`;
- `role-access-mismatch` declares the output role with read-only access;
- `tstore-on-read` selects a test-only HostVector artifact whose first kernel
  stores its result through the normal read-only `a` input binding;
- `tload-on-write` selects a test-only HostVector artifact whose final kernel
  loads through the normal write-only `f` output binding.

The retired-segment protocol binds a map to the complete source export
lifetime identity: `{owner_cna, remote_uba, token_id, generation,
export_mem_id}`.  The acceptance workload supplies `generation` and
`export_mem_id` through the simulator-private OBMM import ABI v3 and its
independent `SIM_DEC_OP_OBMM_MAP_V2` operation.  The ordinary import ABI v1
remains the default for callers that do not request lifetime tracking.  On
unexport, the kernel calls the simulator provider before releasing the
producer backing; a missing provider or failed tombstone publication makes
unexport fail closed.  Every consumer mapping revalidation checks the exact
five-part shared tombstone, so later reuse of the same UBA or token does not
retire a different export generation.

All metadata mutations happen after the public
`lingqu_shmem_pto_dispatch_prepare()` call and remain inside this acceptance
application. The workload recomputes the ABI CRC over the complete control,
memref, shape, and stride metadata before submission. Consequently, a CRC
failure cannot accidentally satisfy the test. Mapping, requester, bounds, and
role/access mismatch cases must fail during QEMU preflight with no binding or
PTO data callback. Public `lingqu_shmem` callers still cannot construct these
test-only wire objects through the ordinary memref API.

The two execution-stage cases keep the ordinary `[INPUT/READ, INPUT/READ,
OUTPUT/WRITE]` metadata and callable signature. Their manifest carries an
explicit `ub_gm_access_fault` marker, and the artifact fingerprint covers the
faulty kernel binary. The host gate rejects a marker/case mismatch before QEMU
starts. A valid run must pass QEMU preflight, register all three bindings,
enter the callable, and fail only when the test kernel's `TSTORE` or `TLOAD`
requests access excluded by the binding. Both classes require one status-3
completion with the exact expected error, one command-slot retirement, and an
unchanged producer sentinel.

Callable 1 currently identifies the frozen host-vector artifact whose PTO
kernel executes one `128 x 128` `f32` tile. For each element it computes
`c = a + b`, followed by `f = (c + 1) * (c + 2)`. The producer verifies this
callable-specific result in its original export mapping. The workload rejects
any other element count before exporting or importing memory, so a shorter
memref cannot reach the kernel and fail during its fixed-size `TLOAD`.
