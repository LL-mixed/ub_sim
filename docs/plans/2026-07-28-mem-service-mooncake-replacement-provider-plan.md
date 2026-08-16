# Memory Service Mooncake Replacement Provider Plan

## 1. Goal

Build `mem_service` into the complete Mooncake replacement for serving and
training workloads while keeping the service independent of every transport.
The same object, KV, placement, scheduling, and lifecycle semantics must run
over QEMU UB/URMA, shared memory, DGX RoCE, TCP diagnostics, and durable local
storage.

This is capability replacement, not source-level emulation. Production
deployments must not install or call Mooncake.

## 2. Non-negotiable boundary

The service is split into three layers:

1. `mem_service` core owns object identity, catalog, placement, leases,
   replication policy, lifecycle, failure policy, and scheduling.
2. The provider contract owns transport-neutral region registration,
   transfer submission, completion, health, and capability discovery.
3. Provider modules own platform APIs, endpoints, memory keys, queue pairs,
   rails, and retry mechanics.

The core may store a provider name, instance identity, object handle, offset,
length, version, checksum, and bounded opaque descriptor. It may not interpret
the opaque descriptor.

## 3. Readiness contract

The current `shmem_ready`, `urma_ready`, and `block_ready` fields mix service
health with one deployment's mechanisms. Replace them with:

- `control_plane_ready`: catalog and metadata operations are available;
- `provider_registry_ready`: providers can be registered and queried;
- `durable_ready`: configured metadata durability is available;
- `data_plane_ready`: every configured transfer provider reports ready, and
  at least one transfer provider is configured;
- `provider_count` and `provider_ready_count`.

`ready` means the configured service role can accept requests.
`data_plane_ready` separately states whether provider-backed payload transfer
is available. A metadata-only control plane must never claim a specific
transport is ready.

## 4. Provider contract

The first contract revision includes:

- stable provider and instance names;
- capability bits;
- provider health probing;
- local region registration and deregistration;
- bounded source and destination slices;
- asynchronous transfer submission;
- completion polling;
- neutral error mapping;
- a bounded opaque region descriptor.

The registry rejects duplicate `(provider, instance)` identities, invalid
operations, oversized descriptors, and providers that claim ready without all
operations required by their capabilities.

## 5. Phase sequence

### Phase 1: neutral core

- Add the provider contract and registry.
- Replace transport-named readiness with neutral readiness.
- Add CLI provider fixtures and provider status.
- Add mock-provider conformance and fail-closed tests.
- Keep existing payload backends operational but mark them as unported.

### Phase 2: extract existing providers

- Move UB SSD/GSVA code out of the core build.
- Extract OBMM shared-memory and URMA runtime access behind providers.
- Move TCP block transport behind a diagnostic provider.
- Run the QEMU eight-node PP regression after each extraction.

### Phase 3: DGX data plane

- Implement a separately linked RoCE provider.
- Discover the configured three-node full mesh without exposing rails to the
  service core or DS4.
- Validate all three node pairs with the common conformance and benchmark CLI.
- Add pinned host-memory support before attempting accelerator-direct memory.

### Phase 4: DS4 connector

- Add a process-resident neutral data-plane SDK. DS4 owns its activation
  buffers, so region registration runs inside DS4; the daemon remains the
  control plane and observability endpoint.
- Replace DS4's direct distributed payload calls with the Memory Service SDK.
- Keep DS4 control frames transport-independent by exchanging only serialized
  opaque region descriptors and completion metadata.
- Move PP activation, KV shards, snapshots, and expert tiles through neutral
  object and transfer requests.
- Keep payloads peer-to-peer and keep the control plane out of the hot path.

### Phase 5: full Mooncake capability replacement

- Add global scheduling, cache-aware placement, replication, tiering,
  eviction, recovery, membership, leases, and observability.
- Validate PP, PD, EP, checkpoint, prefix/KV reuse, and failure recovery
  without a Mooncake dependency.

## 6. Phase 1 acceptance

- Core source contains no transport-specific readiness field.
- Host status does not report URMA, shared memory, or block readiness.
- A service with no registered provider reports `data_plane_ready=0`.
- A ready mock provider changes `data_plane_ready` to `1`.
- Duplicate providers and invalid capability/operation combinations fail.
- The provider fixture is available through the CLI.
- Rust workspace tests and all guest Python tests pass.
- The dgx1 canary remains isolated from the DS4 service on port 8000.
