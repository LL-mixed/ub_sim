# W5 Lingqu Memory Service Design

## Goal

Define a real long-term memory, semantic-state, and inference execution memory
service for W5 inference cluster validation.

This service is not an Engram-owned subsystem. Engram is one consumer. The
memory service must also be usable later by RAG-style context injection, KV
summary state, planner state, session recall, cross-run analysis, and
shortpath-aware range execution.

The hard boundary is:

- hot runtime tensors use the OBMM shmem pool and Lingqu Object Service;
- durable payloads use Lingqu Block;
- durable namespace and metadata catalogs use Lingqu DFS;
- W5 decode consumes ready object references and mapped operand views;
- decode kernels do not ingest, rank, persist, or directly read DFS/Block.
- range boundary shortpath decisions consume ObjectRefs and model-bound
  execution artifacts; they do not consume raw vector database rows.

## Core Decision

Use four explicit layers:

```text
Lingqu Memory Service
  durable memory records, chunks, embedding segments, vector indexes,
  retrieval policy, trust policy, query results, execution artifacts,
  boundary lookup, and shortpath decision records

Hot State Materializer
  converts retrieval results into OBMM-backed tensor objects and publishes
  them through Lingqu Object Service

W5 Engram Adapter
  converts hot memory state into the operator-specific EngramStateObject
  consumed by the W5 engram context op

W5 Boundary Planner
  resolves model-bound execution artifacts at each range exit and returns
  continue/jump/verify decisions; at each range start it can issue range,
  step, or multi-step prefetch plans
```

Lingqu Object Service remains the authority for object identity, placement,
version, checksum, owner, and lifecycle. It is not the semantic retrieval
engine and not the shortpath policy engine.

## Shortpath Direction

The service must not stop at:

```text
query embedding -> hot embedding table -> terminal engram context op
```

That path is useful for proving real ObjectRef wiring, but it cannot reduce
TTFT or range compute because it runs after the transformer range pipeline has
already paid the cost.

The target path is:

```text
range exit hidden_ref + engram_state_ref
  -> BoundaryLookupRequest
  -> model-bound ExecutionArtifact lookup
  -> ShortpathDecisionRecord
  -> continue | jump_to_layer | jump_to_terminal | require_verify
```

There is a second target path for latency hiding:

```text
range start + engram_state_ref + optional previous hidden/KV refs
  -> PrefetchPlanRequest
  -> PrefetchPlanRecord
  -> range prefetch | step prefetch | n-step prefetch
```

Shortpath is a skip decision made at a range exit. Prefetch is a scheduling
decision normally made at a range start. They can use the same semantic memory
and execution artifact indexes, but they should not share the same record type:
prefetch may only make future data cheaper, while shortpath changes the
executed layer/step path and needs stronger proof.

The Memory Service therefore owns two related but distinct domains:

- semantic memory: records, chunks, embeddings, indexes, query results, hot
  memory tensors, and `EngramStateObject`;
- execution memory: verified hidden/KV/logits artifacts, model/tokenizer
  bindings, boundary indexes, and shortpath decisions.

Embedding search can help find relevant state, but a shortpath jump must be
backed by a model-native execution artifact with a model binding, layer range,
position, shape, checksum, confidence, and verification state. A vector hit by
itself is not enough to skip downstream transformer ranges.

## Memory Service Versus Vector Database

If Lingqu Memory Service only stores embedding tables, runs top-k vector
search, and returns ids plus scores, then it is just a small vector database.
That is not the target architecture.

A vector database solves this narrower problem:

```text
vector -> nearest neighbors
```

Its useful responsibilities are:

- store vector rows;
- build exact or approximate indexes;
- run kNN or ANN search;
- return ids, scores, and metadata.

Lingqu Memory Service solves a broader long-term memory problem:

```text
durable semantic memory
  -> audited retrieval result
  -> hot runtime tensor state
```

Its responsibilities are:

- decide which source content can become long-term memory;
- preserve source provenance, evidence refs, trust, retention, security, PII,
  expiry, quarantine, and version state as first-class fields;
- persist memory catalogs through Lingqu DFS and payload bytes through Lingqu
  Block;
- own retrieval policy and query results, not just vector similarity;
- convert selected memory into OBMM-backed hot tensor objects through the Hot
  State Materializer;
- expose `HotMemoryStateObject` and `EngramStateObject` refs that W5 decode can
  resolve without reading DFS or Block;
- report exactly which memory records, chunks, index versions, object refs,
  checksums, and policies affected a decode run;
- control feedback/writeback so model-generated content does not silently
  become trusted memory.

The vector index is an implementation backend inside the Memory Service. It
can start as flat exact search over `EmbeddingSegment` pages, and later become
HNSW, IVF, a Lingqu-native index, or an external vector database. Swapping the
index backend must not change memory policy, audit semantics, hot-state object
contracts, or W5 decode inputs.

Traditional vector database output looks like:

```text
[{ id: chunk_id, score, metadata }]
```

Lingqu Memory Service output must remain structured around memory and runtime
contracts:

```text
QueryResult {
  selected_memory_ids,
  selected_chunk_ids,
  vector_index_id,
  embedding_segment_versions,
  trust_policy_result,
  evidence_refs,
  checksum,
}

HotMemoryStateObject {
  table_object_ref,
  indices_object_ref,
  score_object_ref,
  dtype,
  shape,
  object_versions,
  object_checksums,
}

EngramStateObject {
  hot_state_id,
  query_result_id,
  operator_kind,
  operator_config_hash,
  compatible_model_bindings,
  table_object_ref,
  indices_object_ref,
  gate_feature_object_ref,
  gate_weight_object_ref,
  dtype,
  hidden_size,
  table_rows,
  execution_artifact_index_ref,
  checksum,
  version,
}
```

Therefore:

- Vector DB is a retrieval engine.
- Lingqu Memory Service is a long-term memory system.
- Engram is a consumer of hot memory state, not the owner of memory
  persistence.

If an implementation only exposes `insert_embedding` and `search_embedding`,
rename it to `VectorStore`; it is not the Lingqu Memory Service described by
this design.

## Why

Short W5 decode runs can validate wiring and timing, but they cannot create a
meaningful long-term memory source by themselves. A real memory service gives
W5 a durable and auditable source of semantic state:

- cross-session records;
- long-sequence summaries;
- source provenance;
- trust and retention policy;
- persisted embedding/index artifacts;
- hot OBMM tensor state for low-latency decode consumption;
- rebuild after OBMM eviction or simulator restart.

The user-facing benefit is that W5 reports can identify exactly which memory
records, index versions, object refs, and checksums affected a decode run.

## Non-Goals

- Do not build a production vector database in the first slice.
- Do not let the Memory Service collapse into a generic vector database API.
- Do not put ingestion, ranking, indexing, or persistence into the decode
  kernel.
- Do not make deterministic synthetic engram fixtures the default for real W5
  validation.
- Do not replace Lingqu Object Service, Lingqu Block, or Lingqu DFS with a
  separate storage stack.
- Do not let model output silently become trusted long-term memory.
- Do not claim quality improvements from two-step or four-step decode runs.
  Those runs only validate plumbing and timing.

## Storage Roles

### Lingqu DFS

DFS stores durable catalogs and human-meaningful namespaces:

- memory corpus catalogs;
- source document paths;
- session and run-log manifests;
- metadata checkpoints for rebuilding Object Service runtime indexes;
- vector index manifests;
- replay manifests.

DFS is the durable namespace and metadata-catalog layer. It should not be used
as the primary storage path for large dense vector pages when Lingqu Block can
store those payloads directly.

### Lingqu Block

Block stores durable payload bytes:

- source chunks;
- normalized text payloads;
- embedding segment pages;
- vector index binary pages;
- table snapshots for replay;
- run-summary payloads;
- checksum-addressed artifacts.

Block refs are the durable payload references used by Memory Service records.

The durable simulation backend for DFS and Block is specified separately in
`docs/plans/2026-05-18-lingqu-block-dfs-durable-simulation-design.md`. Memory
Service should treat that backend as the durable substrate. It should not grow
its own long-term payload store or keep adding feature-specific registry JSON
files as durable state.

### Lingqu Object Service

Object Service owns runtime object semantics:

- object identity;
- version selection;
- placement selection;
- checksum verification;
- owner and lifecycle state;
- publish and resolve reports.

Object Service can track `ObmmShmem`, `Block`, and `Dfs` placements, but it
does not own semantic ranking or memory policy.

### OBMM Shmem Pool

OBMM stores hot runtime tensor bytes:

- selected table tensors;
- selected index tensors;
- optional retrieval score tensors;
- adapter-produced gate feature tensors;
- per-query hot state;
- per-node mapped operand caches.

OBMM eviction must never lose durable memory. Eviction only drops a hot
placement. The Memory Service can rebuild hot state from DFS catalogs and Block
payloads.

## Architecture

```text
source docs / session logs / run summaries / user feedback
  -> Lingqu Memory Ingestion
  -> DFS MemoryCorpusCatalog + Block chunk payloads
  -> Embedding Builder
  -> Block EmbeddingSegment payloads
  -> Vector Index Backend
  -> DFS VectorIndexObject manifest + optional Block index pages
  -> Lingqu Memory Query
  -> QueryResult with memory ids, record/chunk ids, policy result, and vector offsets
  -> Hot State Materializer
  -> OBMM-backed HotMemoryStateObject refs via Object Service
  -> W5 Engram Adapter
  -> EngramStateObjectRef
  -> W5 decode resolves refs through UAPI adapter
  -> backend reads mapped OBMM buffer views
```

Only the hot materialization path touches OBMM. Durable memory is defined by DFS
catalogs and Block payloads, not by whether a buffer is currently resident.

For shortpath execution, the range pipeline adds a separate boundary path:

```text
nodeN range compute
  -> publish hidden_ref / kv_ref through Lingqu Object Service
  -> BoundaryLookupRequest {
       model binding,
       step/layer boundary,
       hidden_ref,
       engram_state_ref,
       allowed_actions
     }
  -> Lingqu Memory Service resolves verified ExecutionArtifactObject
  -> ShortpathDecisionRecord
  -> continue normal handoff
     | jump to downstream hidden/KV artifact
     | jump to terminal logits/token artifact
     | require shadow verification
```

This makes every range exit boundary a possible decision point. The decision
must be auditable; W5 reports need the request id, artifact id, model binding,
confidence, checksum, and verification policy that affected the run.

## Data Model

### MemoryCorpusCatalog

Durable DFS metadata checkpoint for a corpus or scope.

```text
catalog_id: string
scope: tenant | user | project | session | run
dfs_path: string
version: u64
record_count: u64
chunk_count: u64
embedding_segments: [embedding_segment_id]
vector_indexes: [vector_index_id]
block_refs: [Lingqu Block ref]
checksum: u64
created_at_us: u64
updated_at_us: u64
```

This catalog is the durable reconstruction source for Memory Service metadata
after restart. Object Service runtime records can be rebuilt from these DFS
catalogs plus Block refs.

### MemoryRecord

Logical durable memory item.

```text
memory_id: string
scope: tenant | user | project | session | run
visibility: private | project | shared | system
source_kind: document | session_log | run_summary | user_feedback | derived
source_uri: string
source_checksum: u64
content_type: text | markdown | json | binary
language: optional string
token_count: u32
trust_level: raw | derived | user_confirmed | system_verified
confidence: f32
retention_policy: ephemeral | session | project | durable
security_label: public | internal | sensitive | restricted
pii_state: unknown | absent | present | redacted
chunk_refs: [chunk_id]
embedding_model_versions: [string]
created_at_us: u64
updated_at_us: u64
expires_at_us: optional u64
version: u64
state: committed | tombstoned | quarantined
```

`metadata: map` can exist as an extension field, but the fields above must be
first-class because retrieval, permissions, retention, and trust depend on
them.

### MemoryChunk

Durable chunk of a memory record.

```text
chunk_id: string
memory_id: string
ordinal: u32
content_block_ref: Lingqu Block ref
content_checksum: u64
token_start: u32
token_count: u32
created_at_us: u64
version: u64
```

Chunk payloads live in Lingqu Block. DFS catalogs name and group them.

### EmbeddingSegment

Durable dense-vector storage unit.

```text
embedding_segment_id: string
embedding_model: string
embedding_model_version: string
dims: u32
dtype: f16 | bf16 | f32
normalized: bool
row_count: u32
row_stride_bytes: u32
vector_block_refs: [Lingqu Block ref]
row_map: [(chunk_id, row_offset)]
checksum: u64
version: u64
created_at_us: u64
```

Do not store one tiny Block object per vector as the normal path. The normal
path is segment/page-based storage so query can read contiguous vector pages and
hot-promote useful ranges into OBMM.

### VectorIndexObject

Durable index manifest and optional hot placement descriptor.

```text
vector_index_id: string
scope: tenant | user | project | session | run
embedding_model: string
dims: u32
index_kind: flat | hnsw | ivf | deterministic_small
segment_ids: [embedding_segment_id]
index_manifest_dfs_path: Lingqu DFS path
index_block_refs: [Lingqu Block ref]
hot_index_object_ref: optional Lingqu ObjectRef to ObmmShmem payload
checksum: u64
version: u64
created_at_us: u64
updated_at_us: u64
```

For the first implementation, `deterministic_small` or `flat` is enough. The
model still needs this object so future approximate indexes do not require a
data-model rewrite.

### Vector Index Backend

The vector index backend is replaceable. It is not the Memory Service public
contract.

Allowed backend shapes:

- `flat`: exact scan over Block-backed `EmbeddingSegment` pages;
- `deterministic_small`: fixture-only index for wiring and timing tests;
- `hnsw` / `ivf`: future Lingqu-native approximate indexes;
- external vector database adapter, if needed.

Backend output is normalized into `QueryResult`. The backend must not decide
memory trust, retention, PII policy, quarantine state, writeback policy, or W5
hot-state layout. Those decisions stay in Lingqu Memory Service and its
materializer/adapter boundary.

### MemoryQuery

Request handled by Lingqu Memory Service or by a request planner before decode.

```text
query_id: string
request_id: string
session_id: string
model_key: string
prompt_ref: Lingqu Block ref or DFS path
prompt_hash: u64
scope_filter: [scope]
visibility_filter: [visibility]
trust_filter: [trust_level]
top_k: u32
embedding_model: string
rank_policy: string
created_at_us: u64
```

This object is not a decode-kernel input. It belongs to the request planning or
memory query phase.

### QueryResult

Durable and auditable retrieval result.

```text
query_result_id: string
query_id: string
vector_index_id: string
selected_chunks: [(chunk_id, embedding_segment_id, row_offset, score)]
selected_memory_ids: [memory_id]
trust_policy_result: string
evidence_refs: [Lingqu Block ref or DFS path]
embedding_segment_versions: [(embedding_segment_id, version)]
record_manifest_dfs_path: Lingqu DFS path
checksum: u64
version: u64
created_at_us: u64
expires_at_us: optional u64
```

`QueryResult` is semantic. It does not have to be shaped like the engram
operator.

Vector scores are only one input to this object. The result must also carry
memory ids, chunk ids, index and segment versions, evidence, checksum, and the
policy decision that allowed the selected memories to be used.

### HotMemoryStateObject

Hot OBMM-backed state produced by the materializer.

```text
hot_state_id: string
query_result_id: string
table_object_ref: Lingqu ObjectRef to ObmmShmem payload
indices_object_ref: Lingqu ObjectRef to ObmmShmem payload
score_object_ref: optional Lingqu ObjectRef to ObmmShmem payload
record_manifest_ref: Lingqu DFS path
dtype: f32
hidden_size: u32
table_rows: u32
checksum: u64
version: u64
created_at_us: u64
expires_at_us: u64
```

This is the memory-facing hot contract. It is still operator-neutral.

### EngramStateObject

W5 Engram Adapter output.

```text
engram_state_id: string
hot_state_id: string
query_result_id: string
query_result_manifest_ref: optional Lingqu DFS path
operator_kind: context_gate
operator_config_hash: u64
compatible_model_bindings: [InferenceModelBinding]
table_object_ref: Lingqu ObjectRef to ObmmShmem payload
indices_object_ref: Lingqu ObjectRef to ObmmShmem payload
gate_feature_object_ref: optional Lingqu ObjectRef to ObmmShmem payload
gate_weight_object_ref: optional Lingqu ObjectRef to ObmmShmem payload
dtype: f32
hidden_size: u32
table_rows: u32
execution_artifact_index_ref: optional Lingqu DFS path
checksum: u64
version: u64
created_at_us: u64
expires_at_us: optional u64
```

`gate_weight`, `gate_feature`, bias, and other gating details belong to the W5
Engram Adapter or operator configuration, not to the core Memory Service.

`EngramStateObject` is a materialized semantic/hot-memory view. It is not a
shortpath decision record and must not encode `continue`, `jump_to_layer`, or
`jump_to_terminal` directly. Range execution passes the `engram_state_id` as
one input to `BoundaryLookupRequest`; the Memory Service or boundary planner
then decides whether a verified `ExecutionArtifactObject` can be used.

`compatible_model_bindings` is optional at materialization time but mandatory
for production shortpath use. An unbound Engram state can drive terminal
context augmentation, but it cannot justify skipping model layers because the
service cannot prove compatibility with weights, tokenizer, or range layout.

The older per-step `Qwen3EngramState` used by the CLI remains a sampling and
policy audit structure. It records selected tokens, blocked candidates, and
checksums; it is not the state model for boundary shortpath.

### InferenceModelBinding

Model identity attached to every execution artifact and boundary lookup.

```text
model_id: string
model_key: string
tokenizer_hash: u64
profile_hash: u64
```

The same semantic memory can be embedded once and queried by many consumers,
but execution artifacts are model-native. Reusing a hidden/KV/logits artifact
across different model weights, tokenizer state, or profile layout is invalid.

### RangeBoundary

Range boundary where a planner can make either a range-start prefetch decision
or a range-exit shortpath decision.

```text
phase: range_start | range_exit
step_index: u64
node_index: u32
layer_start: u32
layer_end: u32
next_node_index: optional u32
position: u64
```

For the current 8-node Qwen3 range pipeline, a boundary exists after each node
publishes its range output, and a paired boundary exists before each node starts
its assigned layer range. Later pipelines can add finer grain boundaries, but
the contract should stay the same.

`range_exit` is the only valid phase for `BoundaryLookupRequest`.
`range_start` is the normal phase for `PrefetchPlanRequest`.

### ExecutionArtifactObject

Durable or hot model-native artifact that can justify a shortpath jump.

```text
artifact_id: string
kind: hidden_state | kv_cache | logits
model: InferenceModelBinding
producer_boundary: RangeBoundary
target_layer_start: u32
target_layer_end: u32
dtype: f16 | bf16 | f32 | u32 | ...
shape: [u64]
durable_payload_ref: optional Lingqu Block ref
hot_object_ref: optional Lingqu ObjectRef to ObmmShmem payload
source_query_result_id: optional string
source_engram_state_id: optional string
confidence_milli: u32
state: candidate | verified | rejected
checksum: u64
version: u64
created_at_us: u64
expires_at_us: optional u64
```

Artifact kinds:

- `hidden_state`: can skip one or more downstream layer ranges by publishing a
  downstream hidden object;
- `kv_cache`: can reuse prefix/session/memory KV blocks during prefill or
  decode, but it must not be treated as `jump_to_layer` by boundary lookup;
  prefix reuse has its own lookup/reuse plan below;
- `logits`: can jump to terminal sampling when the boundary state is verified
  or accepted by policy.

`candidate` artifacts may be produced by speculative paths, but W5 must not
use them for non-shadow jumps unless the decision explicitly requires
verification. `verified` artifacts can be used for direct jumps if policy and
confidence allow it. `rejected` artifacts remain only for audit.

### PrefixCacheKey

Prefix identity used to prove that a KV cache block is reusable for a new
request. The service should never infer prefix equality from text alone.

```text
model: InferenceModelBinding
namespace: string
chat_template_hash: u64
prefix_token_hash: u64
prefix_token_count: u64
rope_config_hash: u64
kv_layout_hash: u64
layer_start: u32
layer_end: u32
position_start: u64
position_end: u64
security_label: public | internal | confidential | restricted
```

The request planner can build multiple candidate keys for exact and partial
prefixes, ordered by desired specificity. This avoids asking the Memory
Service to guess a shorter prefix hash from a full prefix hash.

### PrefixCacheArtifact

Reusable prefix KV materialization.

```text
artifact_id: string
key: PrefixCacheKey
kv_artifact_ids: [ExecutionArtifactObject id where kind=kv_cache]
durable_payload_refs: [Lingqu Block ref]
hot_object_refs: [Lingqu ObjectRef to ObmmShmem payload]
dtype: f16 | bf16 | f32 | ...
shape: [u64]
confidence_milli: u32
state: candidate | verified | rejected
checksum: u64
version: u64
created_at_us: u64
expires_at_us: optional u64
last_used_at_us: u64
use_count: u64
```

`PrefixCacheArtifact` is model-native execution memory. It is not semantic
memory and not an Engram state. A verified prefix cache artifact lets W5 skip
the covered prefix prefill work by attaching KV state; it does not by itself
authorize a layer jump at a range exit.

### PrefixCacheLookupRequest

Request issued during planning, prefill start, or range start.

```text
request_id: string
candidate_keys: [PrefixCacheKey]
min_confidence_milli: u32
allow_verify: bool
created_at_us: u64
```

The lookup returns the longest valid candidate hit. Candidate artifacts can be
returned only when `allow_verify=true`, and the reuse plan must require
verification before the runtime attaches that KV.

### PrefixCacheReusePlan

Auditable prefix cache reuse decision.

```text
plan_id: string
request_id: string
action: miss | reuse | require_verify
artifact_id: optional string
matched_prefix_token_count: u64
layer_start: u32
layer_end: u32
position_start: u64
position_end: u64
confidence_milli: u32
verify_required: bool
proof_checksum: u64
reason: string
created_at_us: u64
version: u64
```

### PrefixCacheLookupResponse

```text
request_id: string
reuse_plan: PrefixCacheReusePlan
artifact: optional PrefixCacheArtifact
```

Prefix cache lookup can feed prefetch planning: a range-start prefetch plan can
ask the runtime to materialize or map the selected prefix KV artifacts before
the next prefill/decode boundary needs them.

### BoundaryLookupRequest

Request issued at a range exit boundary.

```text
request_id: string
model: InferenceModelBinding
boundary: RangeBoundary
hidden_state: HotTensorObjectRef
engram_state_id: optional string
min_confidence_milli: u32
allowed_actions: [continue | jump_to_layer | jump_to_terminal | require_verify]
created_at_us: u64
```

This request is control-plane metadata plus object refs. It must not contain
large tensor payloads. The Memory Service can resolve object metadata and
artifact indexes; backend kernels still only read mapped buffers.

### PrefetchPlanRequest

Request issued at a range start boundary to hide future ObjectRef, KV, hidden,
or logits materialization latency.

```text
request_id: string
model: InferenceModelBinding
boundary: RangeBoundary with phase=range_start
engram_state_id: optional string
scope: range | step | multi_step
lookahead_steps: u32
artifact_kinds: [hidden_state | kv_cache | logits]
created_at_us: u64
```

`scope=range` means prefetch likely operands for this node's current layer
range. `scope=step` means prefetch one or more artifacts for a future decode
step. `scope=multi_step` is explicit n-step lookahead; it should remain a plan
until the scheduler confirms enough confidence and memory pressure budget.

### PrefetchPlanRecord

Auditable prefetch plan returned by the Memory Service or boundary planner.

```text
plan_id: string
request_id: string
model: InferenceModelBinding
boundary: RangeBoundary with phase=range_start
engram_state_id: optional string
scope: range | step | multi_step
lookahead_steps: u32
target_step_index: u64
target_position: u64
artifact_kinds: [hidden_state | kv_cache | logits]
planned_artifact_ids: [string]
state: planned | issued | completed | cancelled
checksum: u64
version: u64
created_at_us: u64
expires_at_us: optional u64
```

This record does not prove skipped computation. It only proves what the planner
asked the runtime to make cheaper before a later boundary or step needs it.

### ShortpathDecisionRecord

Auditable decision returned by the Memory Service or boundary planner.

```text
decision_id: string
request_id: string
action: continue | jump_to_layer | jump_to_terminal | require_verify
artifact_id: optional string
target_layer_start: optional u32
target_layer_end: optional u32
confidence_milli: u32
verify_required: bool
proof_checksum: u64
reason: string
created_at_us: u64
version: u64
```

`continue` is a real decision, not a fallback. It means no policy-eligible,
verified execution artifact was found for this boundary. A jump decision must
name an `ExecutionArtifactObject`; otherwise the run cannot prove what work was
skipped.

### BoundaryLookupResponse

Boundary lookup result.

```text
request_id: string
decision: ShortpathDecisionRecord
artifact: optional ExecutionArtifactObject
```

The response should be small enough to carry in control-plane logs and UAPI
metadata. Actual hidden/KV/logits bytes remain behind ObjectRefs or Block refs.

## Query And Decode Flow

The recommended W5 validation flow is offline/pre-step materialization:

1. Memory ingestion receives source documents, session transcripts, run
   summaries, or feedback events.
2. Ingestion writes chunk payloads to Lingqu Block and updates DFS
   `MemoryCorpusCatalog` records.
3. The embedding builder writes vector pages to Lingqu Block as
   `EmbeddingSegment` payloads.
4. The index builder writes `VectorIndexObject` manifests to DFS and optional
   index pages to Lingqu Block.
5. A request planner submits `MemoryQuery`.
6. Lingqu Memory Service returns `QueryResult`.
7. Hot State Materializer reads selected segment pages, creates OBMM tensor
   payloads, and publishes `HotMemoryStateObject` through Object Service.
8. W5 Engram Adapter converts `HotMemoryStateObject` into
   `EngramStateObject`.
9. W5 decode receives `EngramStateObjectRef`.
10. UAPI adapter resolves ObjectRefs and maps OBMM buffers.
11. The backend reads mapped views. It does not resolve DFS paths or read
    Lingqu Block directly.
12. Decode reports memory record ids, query result version, object refs,
    checksums, and timing.

Shortpath-enabled W5 adds a boundary flow inside step execution:

1. A node completes its assigned range and publishes the range output
   `hidden_ref` and optional KV refs through Lingqu Object Service.
2. The boundary planner submits a `BoundaryLookupRequest` with the model
   binding, range boundary, `hidden_ref`, `EngramStateObject` id, and allowed
   actions.
3. Lingqu Memory Service resolves only verified and policy-eligible
   `ExecutionArtifactObject` records for that exact model/boundary state.
4. The service writes a `ShortpathDecisionRecord`.
5. `continue` keeps the normal node-to-node handoff.
6. `jump_to_layer` publishes or forwards a downstream hidden/KV artifact ref
   and skips the covered range.
7. `jump_to_terminal` forwards a logits artifact to terminal sampling.
8. `require_verify` allows a speculative jump only when a shadow/full path will
   verify the artifact and record the result.

Online query is allowed later, but it must be modeled as request planning before
the decode step. TTFT reports must separate memory query/build-state time from
decode execution time.

## Feedback And Writeback Policy

Raw interaction logs can be stored as memory sources. Derived long-term memory
must not be trusted just because a model generated it.

Writeback rules:

- raw decode/session logs can be persisted as `source_kind=session_log`;
- model-derived summaries start with `trust_level=derived`;
- high-trust memory requires user confirmation, system verification, or an
  explicit policy rule;
- every derived record must keep evidence refs to source chunks or run logs;
- quarantined or low-confidence records cannot be selected by default queries;
- feedback ingestion is performed by Memory Service workers, not by the engram
  context op.

This avoids poisoning long-term memory with model hallucinations.

## W5 Decode Contract

The decode path should accept:

```text
--engram-state-ref=<object key or wire ref>
```

or an equivalent environment variable during script compatibility.

When a real `EngramStateObjectRef` is provided:

- missing object resolve fails the run;
- checksum mismatch fails the run;
- shape mismatch fails the run;
- fallback to deterministic fixture state is forbidden;
- reports must include selected memory ids, query result id, object versions,
  and checksums.

When no memory service is configured, W5 must not synthesize a hidden fixture
state inside the decode path. Unit tests may publish deterministic payloads into
the object registry, but W5 runtime execution still consumes them only through a
real `EngramStateObjectRef` and checksum-validated object resolves.

## Cross-Node Access

The wire contract should reuse the existing object-backed reference model, such
as `LingquObmmObjectRefWire`.

Target path:

```text
producer publishes OBMM-backed object
  -> Lingqu Object Service commits version/checksum/placement
  -> consumer receives ObjectRef in UAPI descriptor
  -> adapter resolves/maps OBMM placement
  -> backend reads mapped buffer view
  -> output object metadata/checksum is committed
```

There must not be a second guest-only object library with separate truth. Guest
code can hold handles and descriptors, but object identity, placement, and
lifecycle are unified through Lingqu Object Service on top of OBMM shmem.

## Lifecycle And Versioning

- Memory records are append-versioned.
- Chunk payloads are immutable for a content checksum.
- Embedding segments are immutable for a chunk set, embedding model, and model
  version.
- Vector index objects are versioned and can be rebuilt from embedding
  segments.
- Query results are versioned and can expire.
- Execution artifacts are model-bound, checksum-bound, and append-versioned;
  reusing them across model/tokenizer/profile changes is invalid.
- Shortpath decisions are immutable audit records for a boundary request.
- Hot OBMM placements can be evicted independently from durable DFS/Block
  state.
- Tombstoned records are hidden from latest queries but remain auditable by
  exact version when policy allows it.
- Quarantined records cannot satisfy normal query or decode resolve requests.

## CLI Shape

Initial commands should be explicit and testable:

```text
sim-cli lingqu-memory ingest --scope project --source docs/...
sim-cli lingqu-memory embed --scope project --embedding-model ...
sim-cli lingqu-memory build-index --scope project --index-kind flat
sim-cli lingqu-memory query --prompt-file prompt.txt --top-k 8
sim-cli lingqu-memory materialize-hot-state --query-result-id ... --hidden-size 1024
sim-cli w5-inference-cluster \
  --engram-state-ref <EngramStateObjectRef wire hex> \
  --object-registry-dir <qwen3-object-registry-dir>
```

The command names can still change during implementation. The user-facing flow
should keep ingestion, embedding, indexing, query, hot materialization, adapter
state, and decode as separately observable stages.

Current CLI contract:

```text
sim-cli lingqu-memory ingest \
  --catalog <catalog-snapshot.json> \
  --store <durable-store.json> \
  --source <source-file> \
  --catalog-id <catalog-id> \
  --namespace <namespace> \
  --record-id <record-id> \
  --chunk-id <chunk-id> \
  --token-count <count> \
  --embedding-model-version <version>

sim-cli lingqu-memory build-index \
  --catalog <catalog-snapshot.json> \
  --store <durable-store.json> \
  --embedding-json <embedding-vectors.json> \
  --index-id <index-id> \
  --segment-id <segment-id>

sim-cli lingqu-memory query \
  --catalog <catalog-snapshot.json> \
  --store <durable-store.json> \
  --query-embedding-json <query-vector.json> \
  --query-id <query-id> \
  --top-k <count>

sim-cli lingqu-memory list-query-results \
  --store <durable-store.json> \
  [--result-id <query-result-id>]

sim-cli lingqu-memory list-record-lifecycle \
  --store <durable-store.json> \
  [--record-id <record-id>]

sim-cli lingqu-memory list-shortpath-decisions \
  --store <durable-store.json> \
  [--decision-id <shortpath-decision-id>]

sim-cli lingqu-memory list-prefetch-plans \
  --store <durable-store.json> \
  [--plan-id <prefetch-plan-id>]

sim-cli lingqu-memory list-prefix-cache-reuse \
  --store <durable-store.json> \
  [--plan-id <prefix-cache-reuse-plan-id>]

sim-cli lingqu-memory update-record-state \
  --catalog <catalog-snapshot.json> \
  --store <durable-store.json> \
  --catalog-id <catalog-id> \
  --record-id <record-id> \
  --state committed|tombstoned|quarantined \
  --actor <actor-id> \
  --reason <mutation-reason>

sim-cli lingqu-memory materialize-hot-state \
  --catalog <catalog-snapshot.json> \
  --store <durable-store.json> \
  --object-store <legacy-object-service-snapshot.json> \
  --query-result-manifest <dfs-query-result-path> \
  --state-id <hot-state-id> \
  --hot-state <hot-state.json>

sim-cli lingqu-memory materialize-engram-state \
  --store <durable-store.json> \
  --object-store <legacy-object-service-snapshot.json> \
  --hot-state <hot-state.json> \
  --gate-weight-json <gate-weight.json> \
  --state-id <engram-state-id> \
  --engram-state <engram-state.json>

sim-cli lingqu-memory publish-w5-engram-state-ref \
  --store <durable-store.json> \
  --object-store <legacy-object-service-snapshot.json> \
  --engram-state <engram-state.json> \
  --registry-dir <qwen3-object-registry-dir>
```

`ingest` reads a real source file, writes the source bytes into the local
Lingqu Block durable-store snapshot, and writes a catalog snapshot with a
committed `MemoryRecord`/`MemoryChunk`.

`build-index` reads explicit embedding vectors from JSON, writes the vector
payload into the same Lingqu Block durable-store snapshot, and registers a flat
`VectorIndexObject` plus an `EmbeddingSegment`. It intentionally does not
synthesize embeddings from source text.

`query` reads an explicit query embedding from JSON, writes that query vector
into Lingqu Block, ranks the flat index, persists the resulting `QueryResult`
manifest into the durable-store DFS snapshot, and reports selected record/chunk
ids. It intentionally does not embed prompt text inside the query command.
`list-query-results` reads the append-only durable DFS query audit log and can
filter by `query-result-id`; it fails if the audit log or requested result is
missing, so audit inspection cannot silently fall back to per-result manifests.
The other `list-*` audit commands follow the same rule for record lifecycle,
shortpath, prefetch, and prefix-cache reuse logs: they inspect the append-only
DFS audit source directly and fail on missing logs or missing filters.

`materialize-hot-state` reloads a persisted `QueryResult` manifest from the
durable-store DFS snapshot, revalidates it against the current catalog, reads
selected embedding rows from Lingqu Block, publishes hot table/index/score
objects through Lingqu Object Service, checkpoints Object Service metadata and
placement records into the same durable-store DFS snapshot, backs object
payload bytes with Lingqu Block refs, and writes a `HotMemoryStateObject`
manifest. It intentionally does not accept inline tensor values. The
checkpoint is required because hot-state object refs are not meaningful across
CLI stages unless the object metadata, payload refs, and OBMM placement records
are reloadable.

`materialize-engram-state` reloads the hot-state manifest and Object Service
checkpoint from the durable store, validates that hot table/index/score refs
resolve to matching objects, writes the caller-provided gate-weight JSON
payload into Lingqu Block, then publishes a gate OBMM object, updates the
Object Service durable checkpoint, and writes the `EngramStateObject`
manifest. It intentionally does not accept inline gate values on the command
line and does not create a default gate if the gate payload is missing.

`publish-w5-engram-state-ref` is the current W5 adapter bridge. It reloads the
`EngramStateObject` and Object Service checkpoint, resolves table/index/gate
payloads from durable Block-backed Object Service records, writes the qwen3
object registry payloads, and prints the exact
`SIM_QWEN3_GUEST_ENGRAM_STATE_REF` plus
`SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR` environment values needed by the existing
W5 guest runner. This is a compatibility bridge until W5 resolves the unified
Lingqu Object Service directly instead of using the qwen3 registry shim.

`sim-cli w5-inference-cluster` also accepts those values directly as
`--engram-state-ref` and `--object-registry-dir`. Passing them enables W5
engram OBMM context mode and defaults the context op to `cpu-reference` unless
the caller explicitly chooses another real context op such as `simpler-host`.

## Observability

W5 summary should include:

```text
memory_service=lingqu_memory_service
memory_fixture_backed=false
query_id=...
query_result_id=...
vector_index_id=...
hot_state_id=...
engram_state_ref=...
selected_memory_count=...
selected_memory_ids=...
selected_chunk_ids=...
table_object_ref=...
indices_object_ref=...
score_object_ref=...
gate_feature_object_ref=...
execution_artifact_id=...
shortpath_decision_id=...
shortpath_action=continue|jump_to_layer|jump_to_terminal|require_verify
shortpath_confidence_milli=...
shortpath_proof_checksum=...
prefetch_plan_id=...
prefetch_scope=range|step|multi_step
prefetch_lookahead_steps=...
prefetch_target_step_index=...
prefetch_state=planned|issued|completed|cancelled
prefix_cache_lookup_id=...
prefix_cache_action=miss|reuse|require_verify
prefix_cache_artifact_id=...
prefix_cache_matched_tokens=...
prefix_cache_proof_checksum=...
object_versions=...
object_checksums=...
obmm_hot_bytes=...
block_read_count=...
dfs_catalog_reads=...
```

Timing should separate:

- memory query and ranking;
- vector index backend scan/search;
- durable DFS catalog reads;
- durable Block payload reads;
- OBMM hot materialization;
- Object Service publish and resolve;
- boundary lookup and artifact-index lookup;
- prefetch plan lookup and issue latency;
- prefix cache lookup and KV attach planning;
- shortpath decision write;
- UAPI object map;
- backend context-op dispatch.

## Test Plan

Unit tests:

- `MemoryRecord` rejects missing first-class policy fields.
- `MemoryChunk` roundtrips DFS catalog refs and Block payload refs.
- `EmbeddingSegment` maps chunk ids to row offsets.
- `VectorIndexObject` can resolve segment pages without per-vector Block
  objects.
- `HotMemoryStateObject` rejects non-OBMM hot tensor placements.
- `ExecutionArtifactObject` requires a model binding, boundary, checksum, and
  at least one durable or hot payload ref.
- `BoundaryLookupRequest` rejects missing hidden refs and invalid confidence
  thresholds.
- `BoundaryLookupRequest` rejects `range_start` boundaries.
- `PrefetchPlanRequest` rejects non-`range_start` boundaries and zero
  lookahead.
- `PrefixCacheKey` rejects missing tokenizer/template/rope/layout hashes and
  mismatched position ranges.
- `PrefixCacheLookupRequest` returns the longest verified candidate and writes
  an auditable miss when there is no usable prefix.
- `ShortpathDecisionRecord` rejects jump decisions without an artifact id.
- OBMM hot promote and evict keeps durable DFS/Block source valid.
- stale version and checksum mismatch fail resolve.
- tombstoned and quarantined memory records are not selected by normal query.
- model-derived memory cannot become high-trust without evidence and policy.

Integration tests:

- ingest a small corpus, build embeddings, build an index, query, materialize
  hot state, and resolve all hot tensor refs.
- rebuild Memory Service runtime metadata from DFS catalogs and Block payloads.
- run W5 without `EngramStateObjectRef` and verify it fails before decode.
- run W5 with real `EngramStateObjectRef` and verify deterministic fallback is
  not reachable.
- evict OBMM hot tensors, rebuild state from Block/DFS, and rerun W5.
- replay a prior run from DFS manifests and Block payload checksums.
- register a verified logits artifact and verify boundary lookup returns
  `jump_to_terminal`.
- verify no-hit boundary lookup returns an auditable `continue` decision.
- issue a multi-step range-start prefetch request and verify the persisted
  `PrefetchPlanRecord` target step, checksum, and state.
- register a verified prefix KV artifact and verify prefix cache lookup
  returns a `reuse` plan instead of a range `jump_to_layer`.

CLI tests:

- each public command has a smoke test;
- missing DFS source fails with a structured error;
- missing Block payload fails with a structured error;
- `--engram-state-ref` shape/checksum mismatch fails decode.

## Implementation Order

0. Add the Lingqu Block/DFS durable simulation backend described in
   `docs/plans/2026-05-18-lingqu-block-dfs-durable-simulation-design.md`, then
   migrate `LingquMemoryDurableStore` to wrap it instead of keeping private
   DFS/Block payload HashMaps.
1. Add core Rust data models for `MemoryCorpusCatalog`, `MemoryRecord`,
   `MemoryChunk`, `EmbeddingSegment`, `VectorIndexObject`, `MemoryQuery`,
   `QueryResult`, `HotMemoryStateObject`, and `EngramStateObject`.
2. Add DFS catalog persistence and restart-time catalog reload.
3. Add Block-backed chunk and embedding segment payload storage.
4. Add deterministic-small or flat vector index support.
5. Add `sim-cli lingqu-memory` ingest/embed/build-index/query commands.
6. Add hot materialization into OBMM shmem through Lingqu Object Service.
7. Add W5 Engram Adapter from `HotMemoryStateObject` to
   `EngramStateObject`.
8. Teach W5 decode to accept `EngramStateObjectRef`.
9. Make real-memory W5 reject deterministic fallback paths.
10. Add core data models for `InferenceModelBinding`, `RangeBoundary`,
    `ExecutionArtifactObject`, `BoundaryLookupRequest`,
    `ShortpathDecisionRecord`, `BoundaryLookupResponse`,
    `PrefetchPlanRequest`, `PrefetchPlanRecord`, `PrefixCacheKey`,
    `PrefixCacheArtifact`, `PrefixCacheLookupRequest`,
    `PrefixCacheLookupResponse`, and `PrefixCacheReusePlan`.
11. Add Memory Service boundary lookup over verified execution artifacts and
    range-start prefetch planning over artifact indexes.
12. Add Memory Service prefix cache lookup over verified model-bound KV
    artifacts.
13. Teach W5 range exit to issue boundary lookups and consume continue/jump
    decisions.
14. Teach W5 range start to issue range/step/n-step prefetch plans and feed
    them into runtime/Object Service scheduling.
15. Teach W5 prefill/request planning to issue prefix cache lookups and attach
    reusable KV through the UAPI descriptor path.
16. Add long-step, cross-session, and restart/rebuild validation runs.

Current implementation status:

- Steps 1-4 have a baseline implementation in `sim-memory`. `QueryResult` now
  carries selected record/chunk ids, vector index ids, embedding segment
  versions/checksums, evidence refs, and its own checksum/version, so query
  output is an auditable memory decision rather than only a top-k vector list.
  `EngramStateObject` now carries query provenance, operator kind/config hash,
  compatible model bindings, tensor dtype/shape, checksum, version, and
  lifetime metadata. This makes it suitable as a trustworthy boundary lookup
  input, while keeping shortpath actions in `ShortpathDecisionRecord`.
  Step 10 also has a baseline data-model implementation in `sim-memory`:
  `InferenceModelBinding`, `RangeBoundary`, `ExecutionArtifactObject`,
  `BoundaryLookupRequest`, `ShortpathDecisionRecord`,
  `BoundaryLookupResponse`, `PrefetchPlanRequest`, and
  `PrefetchPlanRecord`, `PrefixCacheKey`, `PrefixCacheArtifact`,
  `PrefixCacheLookupRequest`, `PrefixCacheLookupResponse`, and
  `PrefixCacheReusePlan` can be validated, registered, and used for a first
  exact boundary lookup over verified execution artifacts, range-start n-step
  prefetch planning, and auditable prefix cache hit/miss planning. Query
  results, shortpath decisions, prefetch plans, and prefix-cache reuse/miss
  decisions are persisted as append-only durable DFS audit logs and can be
  rebuilt after restart. This is not yet wired into W5 guest range execution.
  Query results can be persisted to and restored from DFS manifests with
  checksum validation, and QueryResult-driven hot materialization now carries
  that DFS manifest ref into both `HotMemoryStateObject` and
  `EngramStateObject`.
- Step 5 now has the first real external commands:
  `sim-cli lingqu-memory ingest`, `sim-cli lingqu-memory build-index`, and
  `sim-cli lingqu-memory query`.
  `ingest` persists real source bytes into a local Lingqu Block durable-store
  snapshot and writes committed record/chunk metadata. `build-index` requires
  caller-provided embedding vectors, writes them into Lingqu Block, and
  registers a flat vector index. `query` requires a caller-provided query
  embedding, writes it into Lingqu Block, ranks the flat index, persists a
  checksum-validated `QueryResult` manifest into the DFS snapshot, and appends
  the immutable query decision to the durable DFS audit log. Reusing a query
  result id with different payload is rejected instead of overwriting history.
  `sim-cli lingqu-memory update-record-state` now provides a durable lifecycle
  mutation path for committed/tombstoned/quarantined records: it reloads the
  catalog from DFS when the external catalog file is missing, versions the
  record and catalog, requires explicit actor/reason metadata, persists the
  updated catalog back to DFS, appends a checksum-validated immutable
  `MemoryRecordLifecycleEvent` to the durable DFS audit log, and normal query
  paths filter non-committed records after restart. Step 6 now also has a real
  `sim-cli lingqu-memory materialize-hot-state` entrypoint that
  consumes the persisted query result manifest, publishes OBMM hot tensors, and
  persists a reloadable Lingqu Object Service checkpoint into durable DFS with
  Block-backed object payload refs so the produced `HotMemoryStateObject` refs
  can be resolved by later CLI stages.
  `sim-cli lingqu-memory materialize-engram-state` now consumes that hot-state
  manifest plus durable Object Service checkpoint, writes gate weights through
  Lingqu Block, publishes the gate OBMM object, updates the durable checkpoint,
  and emits an `EngramStateObject` manifest.
  `sim-cli lingqu-memory publish-w5-engram-state-ref` then converts that
  `EngramStateObject` plus durable Object Service checkpoint into the current
  W5 qwen3 object registry state-ref environment contract.
  `sim-cli lingqu-memory list-query-results` exposes durable query audit log
  inspection from the CLI. `list-record-lifecycle`,
  `list-shortpath-decisions`, `list-prefetch-plans`, and
  `list-prefix-cache-reuse` expose the remaining durable Memory Service audit
  logs without reading legacy manifests or synthesizing missing state.
  `lookup-prefix-cache` persists prefix cache reuse/miss plans into a durable
  DFS audit log so cache optimization decisions survive restart and remain
  analyzable. Embed generation remains a missing product CLI entrypoint.
- Step 6 now has two paths: explicit caller-provided tensor materialization and
  QueryResult-driven materialization that reads selected embedding rows from
  Lingqu Block and publishes OBMM-backed table, index, and score tensors through
  Lingqu Object Service.
- Step 7 has the baseline adapter object construction and gate materialization.
  `LingquMemoryService::materialize_engram_state()` now builds the
  `EngramStateObject` from a `HotMemoryStateObject` and publishes the
  gate-weight tensor as an OBMM-backed Lingqu Object Service object.
  `materialize_engram_state_from_block()` reads the gate-weight payload from
  Lingqu Block before publishing the hot OBMM object, so the current W5
  validation path now exercises durable gate config -> hot object
  materialization instead of passing an in-memory gate vector directly from the
  CLI. The Rust W5 context op can now consume object-ref-backed table,
  indices, and gate-weight payloads from the qwen3 object registry through a
  single `SIM_QWEN3_GUEST_ENGRAM_STATE_REF` manifest ref. The W5 runner now
  requires that state ref, rejects legacy component refs as a non-real
  entrypoint, and forwards the state ref plus
  `SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR` through the QEMU
  environment. The guest validates the state ref, writes it into the UAPI
  object-ref sideband, and fails fast on missing refs instead of silently using
  fixture state. The guest does not read the host registry directly; sim-uapi
  resolves the state manifest on the host side and expands it into the
  table/indices/gate operands. On guest-input execution, sim-uapi now requires
  the descriptor sideband when env refs are present, so env can no longer mask
  a broken guest descriptor. The runner also treats configured context refs as
  a hard validation contract: every node must log `target=uapi_object_ref`, the
  W5 summary must report an `*-object-ref` context mode.
  A real W5 guest context op is no longer allowed to run fixture-backed:
  enabling `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP` without the state ref fails
  before QEMU launch, legacy component refs fail before QEMU launch, the
  guest has the same fail-fast check, and sim-uapi rejects guest-input context
  execution without descriptor refs.
  `w5_engram_object_ref_sideband_0_6b_2step_verify_20260517` passed with
  `engram_context_records=2` and `modes=cpu-reference-object-ref`. The
  state-manifest path also passed as
  `w5_engram_state_ref_0_6b_2step_20260517d` with a single
  `SIM_QWEN3_GUEST_ENGRAM_STATE_REF`, `decode_steps_observed=2`,
  `engram_context_records=2`, and `modes=cpu-reference-object-ref`.
  The
  remaining gap is replacing the host qwen3 object registry shim with direct
  guest OBMM DB payload mapping for the long-lived memory context object; the
  existing per-step 128-byte engram policy state remains a separate writeback
  object.

## Acceptance Criteria

- Lingqu Memory Service is usable without Engram.
- W5 Engram consumes Memory Service through an adapter, not by owning memory
  persistence.
- Object Service is not the semantic retrieval engine.
- Durable metadata can be rebuilt from DFS catalogs.
- Durable payloads live in Lingqu Block.
- Hot tensors are OBMM-backed object refs.
- Execution artifacts are model/tokenizer/profile-bound and cannot be reused
  without a matching binding.
- Every shortpath jump has an auditable decision record and artifact id.
- Real-memory W5 fails on missing object refs, checksum mismatch, or shape
  mismatch.
- Reports identify memory records, chunk ids, query result versions, object
  versions, and checksums that affected decode.
- Fixture-backed engram validation remains possible only when explicitly
  requested and clearly labelled.

## Open Questions

- Which embedding model and dimensions should be the first real persistent
  profile?
- Should the first ranking policy be flat exact search over
  `EmbeddingSegment`, or deterministic top-k over small corpora?
- What retention policy applies to user, project, session, and run scopes?
- Which node owns hot state materialization for multi-node W5 runs?
- Should feedback writeback be synchronous after decode or batched by a Memory
  Service worker?
