# Memory Service Provider Layout

This directory contains transport and storage providers for `mem_service`.
Providers implement the neutral contract from the parent directory; they do
not define object identity, KV semantics, placement policy, wire operations,
or service readiness.

## Layout

- One provider uses `mem_service_provider_<name>.c` and
  `mem_service_provider_<name>.h`.
- A provider-specific executable entry point uses
  `mem_service_provider_<name>_cli.c` and builds as
  `linqu_mem_service_provider_<name>`. Its CLI is diagnostic and operational;
  applications still use the transport-neutral mem service SDK.
- Provider-private tests use the same basename with `_test` before the file
  extension. Cross-provider conformance tests stay in
  `guest-linux/aarch64/tests/`.
- Shared provider helpers are allowed only after two providers need the same
  mechanism. They use the `mem_service_provider_common_*` prefix.
- Provider-backed daemon configuration is a strict text contract consumed by
  the provider CLI. Checked-in examples live under
  `apps/mem_service/configs/providers/<name>/`; machine-local deployment
  instances stay outside the repository.

## Boundaries

- Provider headers may include vendor or platform APIs. Core files must never
  include provider headers.
- Providers receive opaque region and transfer requests through the neutral
  contract. Provider-specific connection keys remain inside the opaque
  descriptor.
- Providers may register capabilities and topology costs. They may not alter
  object metadata or choose model policy.
- A provider must fail closed when it cannot prove region ownership, bounds,
  completion, version, or checksum.
- Build targets opt in to providers explicitly. Adding a source file here must
  not make every `mem_service` binary link that provider.
- The installed source SDK keeps neutral `sdk_sources` free of provider
  dependencies. Provider consumers query
  `payload_provider_<name>_sources` and `payload_provider_<name>_libs` from
  `lingqu-mem-service.pc` and opt in explicitly.
- A provider probe may report device availability, but service data-plane
  readiness requires a completed peer transfer and checksum validation.
- Provider control traffic may exchange opaque descriptors and completions.
  Application payload bytes must use the provider data plane.
- A process may register only memory that it owns or has explicitly mapped.
  Consequently, a model runtime uses the neutral provider SDK in the model
  process for hot-path buffers. A separate daemon remains the control plane
  and must not claim zero-copy ownership of another process's heap.
- Applications exchange only the neutral serialized region descriptor. They
  do not parse provider bytes or include provider headers.
- A data-plane channel binds only when the complete configured transfer
  registry is ready. A healthy edge cannot hide a missing full-mesh peer.
- Connection-oriented providers expose a two-phase server lifecycle:
  `listen` first makes the endpoint reachable, and `accept` completes the
  peer connection only after the application control plane has announced
  readiness. The compatibility `endpoint_open(..., server=true)` operation
  performs both phases for standalone canaries.
- Provider verification and region registration happen only after both peers
  have entered the connection phase. Applications must not use timing delays
  to hide a listener/connect race.

## RoCE Mesh Configuration

`linqu_mem_service_provider_roce mesh-serve --config <path>` accepts a strict
line-oriented file. Unknown, duplicate, malformed, or incomplete fields fail
closed. The fields are:

- `version=1`;
- one local `listen=unix:<path>`;
- optional `store`, `storage_root`, and loopback-only `metrics_listen`;
- `verify_bytes`, `verify_iterations`, and `timeout_ms`;
- one or more
  `endpoint=<server|client>,<local-ip>,<peer-ip>,<port>,<device>` entries.

Every configured endpoint must complete a checked peer transfer before the
daemon starts accepting SDK requests.

Initial providers are expected to be:

- a deterministic loopback provider for contract tests;
- UB/URMA and shared-memory providers for QEMU eight-node PP;
- a RoCE full-mesh provider for DGX PP;
- a TCP diagnostic reference that is never an automatic fallback.
