# Lingqu DB/Object Service Design

## Purpose

This document defines the simulator-side design for a real Lingqu DB/Object
Service. The immediate goal is to move Qwen3 decode-loop state out of ad hoc
host-side structs and into one general service that can later be accessed by
multiple simulated nodes and multiple `simpler` runtimes.

The service is not a generic external database. It is the simulator model of the
Lingqu data-service layer described by `lingqu_db`:

```text
lingqu_db      metadata, object index, versions, subscriptions
lingqu_shmem   hot payload placement and low-latency cross-node tensors
lingqu_block   cold or durable payload placement
lingqu_dfs     namespace-level file objects, out of scope for the first slice
```

## Current State

The current simulator has three separate concepts that need to be unified:

- `DbServiceStub` in `sim-services` models latency and existence of `key ->
  bytes`, but does not store object metadata rich enough for LLM execution.
- `WeightsServiceStub` already composes DB metadata with shmem/block payload
  placement for weights and runtime objects. This is closest to the intended
  direction, but it is weight-centric and not the general service boundary.
- Qwen3 decode-loop state in `sim-uapi` still carries runtime state directly:
  guest token ids, selected samples, hidden tensor reports, and
  `Vec<Qwen3Dense06bLayerKvCache>`.

This is enough for proving single-process math, but not enough for a real
multi-node simulator path. The missing abstraction is a general object service
that owns object identity, versioning, placement, readiness, and access tracing.

## Non-Goals

The first implementation is not trying to build a production Redis clone or a
complete distributed storage engine.

Non-goals:

- Redis text protocol compatibility.
- Strong distributed consensus across physical machines.
- Full payload streaming for every tensor in the first slice.
- Replacing `lingqu_shmem` or `lingqu_block`.
- Moving Qwen3 matmul math into the DB service.

The DB/Object Service decides where objects are, whether they are ready, which
version is visible, and how consumers resolve them. Payload transport remains
owned by shmem/block or by inline simulator payload records.

## Design Principles

- One object identity model for weights, KV cache, hidden tensors, activations,
  partial results, logits, and tokenizer artifacts.
- Metadata and payload are separate. Metadata lives in DB; payload lives inline,
  in shmem, in block, or later in DFS.
- All writes are versioned. Consumers resolve a specific version or the latest
  committed version.
- Publish and resolve must be explicit events, so tests can prove cross-node
  dataflow instead of inferring it from checksums.
- The API is node-native. Every operation carries requester/producer/consumer
  entity identity even when the current implementation runs in one host process.
- The first implementation must have a CLI entry point and tests.

## Core Model

### Object Key

Object keys are stable, human-readable paths. They are not physical placement
addresses.

Examples:

```text
qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0
qwen3/session/{session_id}/kv/layer/00/tile/0/position/00000037/k
qwen3/session/{session_id}/kv/layer/00/tile/0/position/00000037/v
qwen3/session/{session_id}/hidden/boundary/node/0/to/1/step/12
qwen3/session/{session_id}/logits/step/12/full_vocab
qwen3/session/{session_id}/tokens/input
```

Key rules:

- Include model id for immutable model assets.
- Include session id for request-local or conversation-local state.
- Include layer id and node id for layer-pipeline objects.
- Include position for KV cache entries.
- Do not encode physical backend details in the logical key.

### Object Kind

The first implementation should support these object kinds:

```rust
pub enum LingquObjectKind {
    WeightShard,
    KvCacheBlock,
    RuntimeTensor,
    TokenBuffer,
    TokenizerAsset,
    Logits,
    Metadata,
}
```

`RuntimeTensor` covers hidden boundary tensors, activation tiles, partial
attention results, and MLP outputs until a narrower split is needed.

### Payload Backend

Payload backend describes where bytes are stored:

```rust
pub enum LingquPayloadBackend {
    Inline,
    Shmem,
    Block,
    Dfs,
    External,
}
```

First slice:

- `Inline`: small metadata, token ids, checksums, tiny test payloads.
- `Shmem`: hot runtime tensors and hot KV entries.
- `Block`: weight shards and cold KV entries.

`Dfs` and `External` should be represented in the type system but can return
`unsupported` until needed.

### Object Record

The DB metadata row should describe logical identity, version, placement, and
integrity:

```rust
pub struct LingquObjectRecord {
    pub key: String,
    pub kind: LingquObjectKind,
    pub version: u64,
    pub state: LingquObjectState,
    pub producer_entity: u64,
    pub owner_entity: Option<u64>,
    pub bytes: u64,
    pub checksum: u64,
    pub dtype: Option<TensorDType>,
    pub shape: Vec<u64>,
    pub layout: Option<TensorLayout>,
    pub placements: Vec<LingquPayloadPlacement>,
    pub created_at_us: u64,
    pub committed_at_us: Option<u64>,
    pub expires_at_us: Option<u64>,
}
```

States:

```rust
pub enum LingquObjectState {
    Pending,
    Committed,
    Tombstoned,
    Quarantined,
}
```

Placement:

```rust
pub struct LingquPayloadPlacement {
    pub backend: LingquPayloadBackend,
    pub storage_ref: String,
    pub segment: Option<SegmentHandle>,
    pub offset: u64,
    pub bytes: u64,
    pub checksum: u64,
    pub locality: LingquObjectLocality,
}
```

Locality should identify whether the placement is local to a node, in a shared
domain, or globally resolvable.

## API Surface

The service API should be explicit about publish, resolve, read, and versioning.

### Publish

```rust
pub struct LingquObjectPublishReq {
    pub task: Option<TaskKey>,
    pub key: String,
    pub kind: LingquObjectKind,
    pub producer_entity: u64,
    pub expected_version: Option<u64>,
    pub metadata: LingquObjectMetadata,
    pub placements: Vec<LingquPayloadPlacement>,
}
```

Semantics:

- Creates a pending record.
- Verifies placement checksums and byte counts when payloads are present.
- Commits the record if all payload writes are ready.
- Returns a version and object checksum.

For the first Rust implementation, payload write completion can be simulated in
the same call, but the report must still expose separate metadata and payload
events.

### Resolve

```rust
pub struct LingquObjectResolveReq {
    pub task: Option<TaskKey>,
    pub key: String,
    pub requester_entity: u64,
    pub version: LingquObjectVersionSelector,
    pub min_state: LingquObjectState,
    pub preferred_backends: Vec<LingquPayloadBackend>,
}
```

Semantics:

- Finds the requested version.
- Selects the best placement for the requester.
- Emits DB metadata get and backend read events.
- Returns a handle to the placement plus integrity metadata.

Version selectors:

```rust
pub enum LingquObjectVersionSelector {
    LatestCommitted,
    Exact(u64),
    AtLeast(u64),
}
```

### Append Version

KV cache and token buffers need append semantics:

```rust
pub struct LingquObjectAppendReq {
    pub base_key: String,
    pub suffix: String,
    pub producer_entity: u64,
    pub previous_version: Option<u64>,
    pub metadata: LingquObjectMetadata,
    pub placements: Vec<LingquPayloadPlacement>,
}
```

For KV cache, append should create a new position-specific object and also
advance a compact index record:

```text
qwen3/session/{session_id}/kv/index/layer/{layer_id}/tile/{tile_id}
```

The index record stores the latest committed position range and digest.

### Subscribe and Notify

The first implementation can use polling, but the object model should reserve
pub/sub:

```rust
pub struct LingquObjectSubscribeReq {
    pub prefix: String,
    pub requester_entity: u64,
}
```

Needed later for:

- hidden boundary ready notifications,
- KV cache block availability,
- weight hot-placement refresh,
- failure/quarantine events.

## Crate Placement

Recommended landing area:

```text
crates/sim-services/src/lib.rs
  pub mod object

crates/sim-uapi/src/lib.rs
  UapiDescriptor::ObjectPublish(...)
  UapiDescriptor::ObjectResolve(...)
  UapiCommand::SubmitObjectPublish { ... }
  UapiCommand::SubmitObjectResolve { ... }

crates/sim-cli/src/main.rs
  lingqu-object-service scenario command
```

`WeightsServiceStub` should not be deleted immediately. It should become either:

- a thin compatibility layer backed by `LingquObjectServiceStub`, or
- a set of helper builders for weight-specific object publish/resolve requests.

## Qwen3 Decode-Loop Migration

The migration should happen in four controlled slices.

### Slice 1: Service Object Report Without Driving Math

Add `LingquObjectServiceStub` and publish/resolve Qwen3 objects in the decode
loop, while keeping the current math path unchanged.

Objects to publish:

- prompt token buffer,
- embedding hidden,
- per-step sampled token,
- KV cache state snapshots,
- final hidden,
- logits output.

Success criteria:

- decode-loop report contains object publish/resolve counts.
- every generated step has committed token and KV objects.
- tests prove missing object resolve fails with structured status.

### Slice 2: KV Cache Metadata Moves to Object Service

Replace `Qwen3Dense06bIncrementalDecodeState.cache_states` as the primary source
of KV readiness with object-service index records.

Keep the in-memory `Vec<Qwen3Dense06bLayerKvCache>` temporarily as the math
payload cache, but require object-service metadata to authorize reads.

Success criteria:

- incremental decode reads KV index from object service.
- append creates one version per `(layer, tile, position, k/v)`.
- read digest is computed from resolved object records, not from private state
  alone.

### Slice 3: KV Cache Payload Placement

Store KV payload summaries through placements:

- `Inline` for tiny checksum-only unit tests.
- `Shmem` for hot simulated runtime payload.
- `Block` for cold fallback.

The first real payload can still be checksum/shape-only. The important change is
that decode can no longer pretend a KV entry exists unless the object service can
resolve it.

Success criteria:

- deleting or quarantining a KV object forces a miss or recompute path.
- resolving a stale version fails or returns the requested exact old version.
- shmem/block placement choice is visible in the report.

### Slice 4: Weight and Intermediate Tensor Migration

Publish weight shards and hidden boundary tensors through the same service.

Objects:

- weight shards with block-backed payloads,
- hot shmem copies for selected shards,
- hidden boundary tensors between layer ranges,
- attention/MLP partials if they cross node or simpler boundaries.

Success criteria:

- Qwen3 report shows weights, KV cache, and runtime tensors under one service.
- old weight-specific service path is compatibility-only.

## 8-Node Range Forward Integration

After the object service exists, 8-node range forward should be implemented as a
consumer of the service, not as a parallel data path.

Target execution:

```text
prompt tokens
  -> object resolve embedding input
  -> node0 range forward layers 0..3
  -> publish hidden boundary node0->node1
  -> node1 resolve boundary, run layers 4..6
  -> ...
  -> node7 publish final hidden
  -> logits resolve final hidden and produce token
```

Each node range execution should:

- resolve its input hidden object,
- resolve its weight shard objects,
- resolve required KV objects,
- run its assigned layers,
- append KV objects for its layers,
- publish output hidden boundary or final hidden.

The range split must use the existing Qwen3 owner function:

```text
owner_node = layer_id * 8 / 28
```

The object service makes this order natural because a node does not need direct
access to another node's private state. It only needs to resolve committed
objects.

## Multi-Node and Multi-Simpler Access Model

Every request carries:

- `task`,
- `requester_entity`,
- `producer_entity`,
- object key,
- version selector,
- preferred backend list.

This allows the same service to model:

- one host process with 8 logical nodes,
- 8 QEMU guests,
- multiple `simpler` runtime contexts,
- later, multiple UB domains.

Contention and access policy should be modeled through metadata:

- producer owns pending version,
- committed versions are multi-reader,
- append requires expected previous version when strict ordering is needed,
- quarantine makes a version visible but unusable,
- tombstone hides latest lookups but still allows exact-version audit if needed.

## Reports and Observability

Add a report block that can be printed at summary, steps, or verbose levels:

```rust
pub struct LingquObjectServiceReport {
    pub publish_count: u64,
    pub resolve_count: u64,
    pub append_count: u64,
    pub metadata_put_count: u64,
    pub metadata_get_count: u64,
    pub shmem_write_count: u64,
    pub shmem_read_count: u64,
    pub block_write_count: u64,
    pub block_read_count: u64,
    pub committed_object_count: u64,
    pub quarantined_object_count: u64,
    pub missing_resolve_count: u64,
    pub checksum: u64,
}
```

Qwen3 decode-loop step report should expose:

- `object_service_ready`,
- `weight_objects`,
- `kv_objects`,
- `runtime_tensor_objects`,
- `object_publish_count`,
- `object_resolve_count`,
- `object_checksum`.

CLI default output should stay concise. Detailed object rows should require a
parameter such as:

```bash
SIM_QWEN3_DECODE_REPORT=verbose
```

## Failure Semantics

Required structured failures:

- `object_missing`
- `object_pending`
- `object_version_conflict`
- `object_backend_unavailable`
- `object_checksum_mismatch`
- `object_quarantined`
- `object_payload_too_large`
- `object_queue_full`

For KV cache, corruption is a cache miss, not silent correctness drift. A bad KV
object should cause quarantine and recompute fallback once recompute is wired.
Until recompute exists, it should fail loudly.

## Test Plan

Every implementation slice must include CLI and tests.

Unit tests in `sim-services`:

- publish inline object and resolve latest.
- publish shmem object and resolve from another entity.
- publish block object and resolve from another entity.
- exact version resolve returns old version.
- append version detects stale previous version.
- missing key returns `object_missing`.
- quarantined object cannot satisfy normal resolve.

UAPI tests in `sim-uapi`:

- submit object publish/resolve through command queue.
- completion source and status are correct.
- object service respects queue depth.

Qwen3 tests:

- decode-loop publishes token, KV, hidden, logits objects.
- incremental decode resolves KV metadata before using cached KV.
- object counts grow by one token step.
- deleting a required KV object fails the step with a clear error.

CLI tests:

- `lingqu-object-service` command prints a concise service report.
- `qwen3-decode-loop` default output remains summary-only.
- verbose mode includes object service details.

Manual validation:

```bash
cargo test -p sim-services object_ -- --test-threads=1
cargo test -p sim-uapi object_ -- --test-threads=1
cargo test -p sim-uapi qwen3_dense_0_6b_decode_loop_feeds_sampled_tokens_forward -- --test-threads=1
SIM_QWEN3_0_6B_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B \
  cargo run --release -p sim-cli -- qwen3-decode-loop \
  scenarios/mvp_2host_single_domain.yaml 4 "Beijing is "
```

## Implementation Order

### P0: General Object Service Skeleton

Files:

- `crates/sim-services/src/lib.rs`
- `crates/sim-uapi/src/lib.rs`
- `crates/sim-cli/src/main.rs`

Deliverables:

- object record types,
- publish/resolve/append APIs,
- inline/shmem/block placement model,
- service report,
- CLI smoke command,
- unit tests.

### P1: Qwen3 Object Publication

Deliverables:

- decode-loop publishes prompt, token, KV snapshot, hidden, logits objects.
- report exposes object service counters.
- no math behavior change yet.

### P2: KV Metadata Read Path

Deliverables:

- incremental decode requires object-service KV index resolution.
- private `cache_states` becomes derived/reporting state, not authority.
- tests cover missing/quarantined KV.

### P3: 8-Node Range Forward

Deliverables:

- range forward API in `sim-models`.
- node range execution in `sim-uapi`.
- hidden boundary objects published/resolved through object service.
- report proves 7 boundary resolves and 28 real layer executions.

### P4: Weight and Runtime Tensor Unification

Deliverables:

- weight shard resolution backed by object service.
- intermediate attention/MLP/logits objects use the same service when crossing
  node or simpler boundaries.
- `WeightsServiceStub` is compatibility glue or removed if no longer needed.

## Hard Completion Criteria

The service is not considered done until these are true:

- Qwen3 decode-loop can run with object service enabled.
- KV cache append/read readiness is driven by object records.
- Object publish/resolve failures are tested.
- 8-node range forward uses object service for boundary tensors.
- Default CLI output is concise, with verbose details parameterized.
- No `synthetic_stages` regression is introduced by the migration.
