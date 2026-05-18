# Lingqu Block/DFS Durable Simulation Design

## Purpose

Lingqu Memory Service should not own durable bytes through private HashMaps.
Its durable state must sit on top of Lingqu DFS metadata manifests and Lingqu
Block payload refs. The next implementation step is therefore a product-shaped
durable simulation layer for Lingqu DFS and Lingqu Block.

This is not a production filesystem or block device. It is a deterministic,
restartable simulation backend with real payload bytes, metadata, versioning,
checksums, failure semantics, and snapshot import/export. It must be strong
enough that Memory Service, Object Service, W5 prefix cache, execution
artifacts, and replay validation can all depend on the same durable substrate
instead of each module creating its own registry file.

## Current Problem

The current code has three different durability meanings:

- `DfsServiceStub` and `BlockServiceStub` model service latency, queueing, and
  hit/miss status, but do not own durable payload bytes.
- `LingquMemoryDurableStore` stores DFS and Block payload bytes in private
  HashMaps, then exports those bytes through `LingquMemoryDurableStoreSnapshot`.
- Newer Memory Service execution features still use separate CLI registry JSON
  files for execution artifacts and prefix cache artifacts.

That was acceptable for local validation. It is not a stable architecture. The
Memory Service currently proves that data can survive across CLI stages, but it
does so by making Memory Service itself the storage engine. That collapses the
intended boundary between semantic memory, durable namespace, and durable
payload storage.

## Design Goals

- Make Lingqu DFS the owner of durable namespace, manifests, metadata, and
  appendable audit records.
- Make Lingqu Block the owner of durable payload bytes and payload checksums.
- Keep simulation deterministic and easy to snapshot for tests and local CLI
  runs.
- Preserve service timing semantics already modeled by `DfsServiceStub` and
  `BlockServiceStub`.
- Let Memory Service rebuild all runtime indexes from DFS manifests and Block
  refs.
- Remove Memory Service private payload HashMaps from the durable path.
- Avoid adding a generic key-value store abstraction above DFS/Block.
- Support Host deployment first, while keeping the API small enough to wrap for
  Guest deployment later.

## Non-Goals

- Do not implement POSIX filesystem semantics.
- Do not implement a real distributed consensus protocol.
- Do not put semantic memory ranking, trust policy, or vector search into DFS
  or Block.
- Do not make Object Service the durable memory catalog.
- Do not keep adding ad hoc registry JSON files for each Memory Service feature.

## Core Architecture

```text
                 +--------------------+
                 | LingquDurableSim   |
                 | schema/timestamp   |
                 | snapshot/checksum  |
                 +---------+----------+
                           |
             +-------------+-------------+
             |                           |
      +------v------+             +------v-------+
      | LingquDfsSim|             | LingquBlockSim|
      | paths       |             | payload bytes |
      | manifests   |             | block refs    |
      | audit logs  |             | checksums     |
      +------+------+             +------+--------+
             |                           ^
             | manifests may name        |
             | Block refs                |
             +---------------------------+

Memory Service writes/reads DFS manifests and Block payload refs through
LingquDurableSim.

Object Service may record Dfs/Block/Obmm placements, but it uses the same
LingquDurableSim backend instead of owning separate durable bytes.
```

DFS and Block are peers, not layers of each other. DFS stores human-meaningful
paths and manifests. Block stores large or checksum-addressed payload bytes.
DFS manifests name Block refs when they need payload bytes.

The first implementation should live in `sim-services::durable`. That keeps it
near the existing `DfsServiceStub` and `BlockServiceStub` timing/queue models
and avoids introducing another crate boundary before the durable API stabilizes.
After the API is stable, moving it into a dedicated crate is acceptable only if
that removes real dependency pressure.

## Data Model

### LingquDurableSimSnapshot

Top-level local simulation snapshot:

```text
kind: "lingqu_durable_sim"
schema_version: u32
profile: LingquDurableSimProfile
dfs: LingquDfsSimSnapshot
block: LingquBlockSimSnapshot
next_timestamp_us: u64
checksum: u64
```

The top-level checksum covers metadata and payload checksums, not necessarily
duplicated full payload bytes twice.

`kind` and `schema_version` are required. CLI load code must reject unknown
snapshot kinds instead of guessing. During migration, legacy
`LingquMemoryDurableStoreSnapshot` can be detected and imported, but writes
must always emit the new `lingqu_durable_sim` format.

### LingquDfsSimSnapshot

```text
files: [LingquDfsFileRecord]
directories: [LingquDfsDirectoryRecord]
```

`directories` are optional for the first implementation. The first useful
contract is path-addressed files with parent path validation.

The snapshot stores DFS file version history, not just latest values. The
unique key is `(path, version)`. Latest committed is the highest version for a
path whose state is `Committed`. Tombstone and quarantine operations append new
versions; they do not mutate old committed records in place.

### LingquDfsFileRecord

```text
path: String
version: u64
state: Committed | Tombstoned | Quarantined
content_ref: LingquDfsContentRef
bytes: u64
checksum: u64
content_type: Json | Binary | Text | Manifest
created_at_us: u64
updated_at_us: u64
writer: Option<String>
metadata: BTreeMap<String, String>
```

`content_ref` can be inline for small manifests or block-backed for larger
payloads:

```text
Inline(bytes)
Block(LingquBlockPayloadRef)
```

DFS owns file identity and versioning. Block owns bytes when `content_ref` is
block-backed.

DFS paths must be absolute Lingqu paths beginning with `/lingqu/`. Empty path
segments, `.` segments, `..` segments, and trailing slash file paths are
invalid. The first implementation does not need POSIX directory operations, but
it must validate parent path shape so manifests cannot be hidden under ambiguous
paths.

### LingquBlockSimSnapshot

```text
blocks: [LingquBlockRecord]
```

### LingquBlockRecord

```text
block: BlockHash
version: u64
durable_state: Committed | Sealed | Tombstoned | Quarantined
cache_state: Clean | Dirty
bytes: Vec<u8>
checksum: u64
created_at_us: u64
updated_at_us: u64
writer: Option<String>
metadata: BTreeMap<String, String>
```

`durable_state` is the state persisted in the snapshot. `cache_state` is only
the simulated block service writeback state. Memory Service durable payload
eligibility must key off `durable_state`, not `cache_state`. The first
implementation can keep `cache_state` internal if exposing it adds complexity.

The first implementation should use whole-block payload refs:

```text
block: BlockHash
offset: 0
bytes: payload_len
checksum: checksum64(payload)
```

Range refs can remain in the public type because current code already carries
`offset` and `bytes`, but writes should initially create single-payload blocks.

Like DFS files, Block snapshot records keep version history. The unique key is
`(block, version)`. Latest committed is the highest version whose
`durable_state` is `Committed` or `Sealed`.

## Required Types

The implementation should define these concrete types instead of leaving
`options` and `version_selector` informal:

```text
LingquVersionSelector
  LatestCommitted
  Exact(u64)

LingquDfsWriteOptions
  expected_version: Option<u64>
  content_type: LingquDfsContentType
  writer: Option<String>
  metadata: BTreeMap<String, String>
  inline_threshold_bytes: usize

LingquBlockWriteOptions
  expected_version: Option<u64>
  seal: bool
  writer: Option<String>
  metadata: BTreeMap<String, String>

LingquDurableStats
  dfs_reads, dfs_writes, dfs_bytes_read, dfs_bytes_written
  block_reads, block_writes, block_bytes_read, block_bytes_written
  checksum_failures, version_conflicts, missing_refs
```

The error type should be explicit and shared by DFS and Block operations:

```text
LingquDurableError
  InvalidPath
  InvalidBlock
  EmptyPayload
  MissingDfsPath
  MissingBlock
  VersionConflict
  ChecksumMismatch
  RangeOverflow
  Tombstoned
  Quarantined
  Sealed
  QueueFull
  SnapshotCodec
  SnapshotValidation
```

Memory Service can map this into `LingquMemoryError`, but the durable layer
should not depend on Memory Service error types.

## API Shape

The durable simulation backend should expose a small API and keep existing
service timing stubs inside the implementation:

```text
LingquDurableSim::new(profile)
LingquDurableSim::export_snapshot()
LingquDurableSim::import_snapshot(snapshot)
LingquDurableSim::stats() -> LingquDurableStats

LingquDurableSim::dfs_write(path, bytes, options) -> LingquDfsPath
LingquDurableSim::dfs_read(path, version_selector) -> Vec<u8>
LingquDurableSim::dfs_stat(path, version_selector) -> LingquDfsFileRecord
LingquDurableSim::dfs_tombstone(path, expected_version)

LingquDurableSim::block_write(block, bytes, options) -> LingquBlockPayloadRef
LingquDurableSim::block_read(ref) -> Vec<u8>
LingquDurableSim::block_stat(block, version_selector) -> LingquBlockRecord
LingquDurableSim::block_seal(block, expected_version)
LingquDurableSim::block_tombstone(block, expected_version)
```

Memory Service should receive a mutable durable backend handle and use only
these methods for durable bytes.

`dfs_write()` may inline small bytes in DFS or write them through Block based
on `inline_threshold_bytes`. Callers that need Block refs must use
`block_write()` directly.

## Versioning And Consistency

DFS:

- Path writes create a new committed version.
- Callers can pass `expected_version` for compare-and-swap update.
- Reads default to latest committed.
- Tombstoned paths are hidden from normal reads but remain auditable.
- Quarantined paths are readable only through explicit admin/debug APIs.
- `Exact(version)` reads fail if that exact version is tombstoned or
  quarantined unless the caller uses an explicit admin/debug API.
- Multiple committed versions for a path are valid history. Duplicate
  `(path, version)` records are invalid.

Block:

- Payload writes create a versioned block record.
- Sealed blocks cannot be overwritten.
- Mutable dirty blocks are allowed only as service timing/cache state. Memory
  Service durable payloads must read only committed or sealed block versions.
- Reads verify payload length and checksum from `LingquBlockPayloadRef`.
- Duplicate `(block, version)` records are invalid.

Atomicity:

- A DFS manifest write that references a Block payload should write Block first,
  verify checksum, then publish the DFS manifest.
- There is no multi-object transaction in the first implementation.
- Recovery must tolerate orphan Block payloads not referenced by DFS.

## Checksums

Checksums are part of the contract, not debug data.

- Every DFS file record stores bytes and checksum.
- Every Block record stores bytes and checksum.
- Every `LingquBlockPayloadRef` must verify selected payload bytes.
- Every Memory Service manifest stores its own checksum over semantic metadata
  and refs.
- Importing a durable snapshot validates all records before making the backend
  usable.

The checksum algorithm can stay as the current deterministic `checksum64`
helper initially. The API should not expose the algorithm as a product promise.

Snapshot import must recompute each record checksum from bytes, then recompute
the top-level checksum from record metadata and record checksums. It must reject
records where `bytes`, `checksum`, and actual payload length disagree.

## Failure Semantics

The simulation should keep failures explicit and testable:

- missing DFS path;
- missing Block;
- checksum mismatch;
- version conflict;
- quarantined/tombstoned read rejection;
- queue full from the service timing model;
- invalid path or empty payload;
- stale manifest ref.

Failures should surface as structured errors instead of falling back to fixture
data or synthetic payloads.

The durable layer must not synthesize data for missing paths, missing blocks,
invalid checksums, or unknown schema versions. That is a hard product rule
because Memory Service decisions and W5 shortpath/prefix-cache reuse depend on
durable provenance.

## DFS Path Conventions

Memory Service must use stable DFS namespaces so runtime indexes are
rebuildable without scanning arbitrary paths:

```text
/lingqu/memory/catalogs/<catalog-id>.json
/lingqu/memory/query-results/<query-id>.json
/lingqu/memory/hot-states/<state-id>.json
/lingqu/memory/engram-states/<state-id>.json
/lingqu/memory/execution-artifacts/<artifact-id>.json
/lingqu/memory/prefix-cache/<artifact-id>.json
/lingqu/memory/audit/shortpath-decisions/<decision-id>.json
/lingqu/memory/audit/prefetch-plans/<plan-id>.json
```

IDs embedded in paths must be sanitized with a single shared helper. The helper
must be deterministic and reject empty results. Do not let each CLI command
invent its own path encoding.

## CLI Shape

The current CLI accepts separate durable store, object store, execution
registry, and prefix cache registry files. That should converge toward:

```text
sim-cli lingqu-durable init \
  --store <durable-sim.json>

sim-cli lingqu-durable stat \
  --store <durable-sim.json>

sim-cli lingqu-memory ingest \
  --store <durable-sim.json> ...

sim-cli lingqu-memory build-index \
  --store <durable-sim.json> ...

sim-cli lingqu-memory register-execution-artifact \
  --store <durable-sim.json> \
  --artifact <artifact.json>

sim-cli lingqu-memory register-prefix-cache \
  --store <durable-sim.json> \
  --artifact <prefix-cache-artifact.json>
```

The local `--store` remains a JSON file for now, but the JSON file represents
the Lingqu durable substrate, not a Memory Service-private snapshot.

The CLI must write only the new durable sim snapshot format. During migration,
it may read legacy `LingquMemoryDurableStoreSnapshot`, import it into
`LingquDurableSimSnapshot`, and then write the new format on the next save.
There should be no long-term dual-write path.

## Memory Service Integration

`LingquMemoryDurableStore` should become a compatibility wrapper over
`LingquDurableSim`, then be phased down.

Target direction:

- `persist_catalog_snapshot()` writes a DFS file.
- `load_catalog_snapshot()` reads a DFS file.
- `persist_query_result()` writes a DFS file.
- `write_block_payload()` writes a Block record.
- `read_block_payload()` reads and verifies a Block ref.
- execution artifact indexes become DFS manifests;
- prefix cache indexes become DFS manifests;
- shortpath decisions and prefetch plans become appendable DFS audit records.

Memory Service should not store payload bytes in its own fields. It can cache
runtime indexes, but those indexes must be rebuildable from DFS manifests and
Block refs.

The compatibility wrapper must preserve the current `LingquMemoryDurableStore`
public methods until Memory Service and CLI migration are complete. The
implementation behind those methods must delegate to `LingquDurableSim`.

## Object Service Integration

Object Service already knows about placements. With durable simulation:

- OBMM placement remains hot runtime data.
- Block placement names durable payload data.
- DFS placement names metadata or manifest data.
- Object Service snapshots should keep object metadata and placement records,
  not duplicate Memory Service semantic catalogs.

The important boundary is that Object Service resolves object identity and
placement, while Memory Service decides which memory/artifact is semantically
eligible.

## Migration Plan

1. Add `sim-services::durable` with `LingquDurableSim`,
   `LingquDfsSimSnapshot`, `LingquBlockSimSnapshot`, required option/selector
   types, structured errors, stats, and direct unit tests.
2. Add local CLI entrypoints `sim-cli lingqu-durable init/stat` over the new
   snapshot format without changing Memory Service yet.
3. Port `LingquMemoryDurableStore` internals to wrap `LingquDurableSim`,
   preserving its current public methods and existing CLI behavior.
4. Keep existing `LingquMemoryDurableStoreSnapshot` decoding temporarily and
   provide a one-way import into `LingquDurableSimSnapshot`.
5. Change Memory Service CLI load/save helpers so `--store` reads/writes the
   new durable sim snapshot after import.
6. Move execution artifact registry from standalone JSON into DFS manifests.
7. Move prefix cache registry from standalone JSON into DFS manifests.
8. Add restart/rebuild tests that construct a fresh Memory Service from only
   durable sim snapshot data.
9. Remove private DFS/Block payload HashMaps from Memory Service durable code
   after all compatibility tests pass.

## Test Plan

Unit tests:

- DFS write/read roundtrips bytes, version, checksum, and metadata.
- DFS latest committed read ignores tombstoned records.
- DFS expected-version mismatch fails.
- Block write/read roundtrips bytes and payload refs.
- Block read rejects checksum mismatch and range overflow.
- Sealed Block rejects overwrite.
- Snapshot export/import validates record checksums.
- Import rejects duplicate latest versions or invalid records.

Integration tests:

- Existing Memory Service ingest/build-index/query/materialize flow survives a
  durable sim snapshot export/import between every stage.
- QueryResult DFS manifest survives restart and can rebuild selected chunks.
- Hot state can be rebuilt from Block embedding pages after OBMM eviction.
- Execution artifact register/boundary lookup works after durable sim restart.
- Prefix cache register/lookup works after durable sim restart.
- Missing Block payload causes a hard structured failure.
- Missing DFS manifest causes a hard structured failure.

CLI tests:

- `lingqu-durable init` creates an empty valid store.
- `lingqu-durable stat` reports DFS file count, Block count, total bytes, and
  checksum.
- `lingqu-memory` commands keep passing with the new store format.
- Legacy store import test covers the current snapshot format during migration.

## Acceptance Criteria

- Memory Service durable payload bytes are no longer stored in Memory Service
  private HashMaps.
- DFS and Block simulation snapshots are independently validated.
- A single durable sim snapshot can carry Memory Service catalog/query data,
  execution artifact manifests, prefix cache manifests, and payload bytes.
- All Memory Service CLI stages can be restarted from the durable sim snapshot.
- No command silently fabricates payloads, manifests, object refs, or artifact
  refs when durable data is missing.
- Existing workspace tests pass, and new restart/rebuild tests cover the new
  durable boundary.

## Implementation Status

Current code status:

- `sim-services::durable` provides the first durable simulation backend with
  DFS file records, Block records, version selectors, checksum validation,
  tombstone/seal handling, and JSON snapshot import/export.
- DFS namespace discovery is available through `dfs_list()` and
  `sim-cli lingqu-durable list`.
- Append-only DFS logs are available through `dfs_append_log_append()` /
  `dfs_append_log_read()` and `sim-cli lingqu-durable append-log/read-log`.
  Records carry monotonically increasing sequence numbers and checksum-chain
  validation.
- Atomic batch commit is available through `commit_batch()` and
  `sim-cli lingqu-durable batch`. The implementation stages the full batch on
  an imported snapshot and swaps it into the live store only after every
  operation succeeds.
- `sim-cli lingqu-durable init/stat/validate` writes, summarizes, and validates
  the `lingqu_durable_sim` snapshot format.
- `LingquMemoryDurableStore` is now a compatibility wrapper over
  `LingquDurableSim`; Memory Service durable bytes are no longer owned by
  private DFS/Block payload HashMaps.
- `lingqu-memory --store` reads legacy `LingquMemoryDurableStoreSnapshot`
  only as a migration input. The next save writes the new
  `lingqu_durable_sim` format.
- Memory Service catalog snapshots, query results, execution artifact
  manifests, prefix cache manifests, shortpath decision audit data, and
  prefetch plan audit data are written into durable DFS.
- Shortpath decision audits and prefetch plan audits use append-only DFS logs
  at `/lingqu/memory/audit/shortpath-decisions.log` and
  `/lingqu/memory/audit/prefetch-plans.log`. Repeated persistence of the same
  already-logged record is idempotent only when the payload is byte-identical;
  the same record id with different payload is a hard validation error.
- The external `--catalog` JSON file remains as a compatibility output/input,
  but `ingest` and `build-index` persist the catalog into durable DFS, and
  `query` / `materialize-hot-state` can restart from `--store` plus
  `--catalog-id` after the external catalog file is removed.
- Execution artifact and prefix cache registry files have been replaced by DFS
  manifests at `/lingqu/memory/execution-artifacts/manifest.json` and
  `/lingqu/memory/prefix-cache/manifest.json`.
- Consumer CLI commands that require durable manifests now fail on missing DFS
  manifests instead of silently treating them as empty registries.

Validated coverage:

- Durable sim unit tests cover DFS read/write, large DFS payloads backed by
  Block records, checksum mismatch, range overflow, tombstone visibility,
  sealed Block overwrite rejection, version conflicts, duplicate snapshot
  versions, missing block-backed DFS payloads, DFS namespace listing,
  append-log checksum-chain replay/corruption, and batch commit rollback.
- Memory Service tests cover durable snapshot restart for catalog/query/hot
  state materialization, execution artifacts, prefix cache artifacts,
  append-log-backed shortpath decision audits, append-log-backed prefetch plan
  audits, and hard failure on missing embedding Block payloads.
- CLI tests cover durable init/stat/list/validate, append-log/read-log, durable
  batch commit, legacy store migration, manifest-backed execution artifact and
  prefix cache flows, durable catalog restart for query and hot-state
  materialization, and missing manifest hard failures.

Remaining work:

- Object Service still uses its own snapshot file for hot OBMM placements.
  Durable Block/DFS simulation is now unified for Memory Service bytes, but
  Object Service placement metadata has not yet been folded into a single
  durable store file.
- The external catalog file is still supported for compatibility. The durable
  DFS catalog path is now the recovery source, but the CLI contract should
  eventually make `--catalog-id` the primary selector and make `--catalog`
  optional or purely an export path.
