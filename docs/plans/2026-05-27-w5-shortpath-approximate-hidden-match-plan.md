# W5 Shortpath Approximate Hidden Match Plan

## Background

Current W5 shortpath boundary lookup is an exact-match mechanism.

The Memory Service path accepts a `BoundaryLookupRequest` only at
`RangeExit`. It then filters verified execution artifacts by model, exact
producer boundary identity, exact output-hidden fingerprint, confidence
threshold, optional Engram state binding, and allowed action. The current
`BoundaryTensorFingerprint` contains:

- `bytes`
- `checksum`
- `dtype`
- `shape`

The guest boundary-registry fast path currently uses a narrower runtime
fingerprint: `boundary_hidden_bytes` and `boundary_hidden_checksum`. It first
matches boundary identity by decode step, producer position, producer layer
start, and producer layer end. It then compares the current range output
hidden bytes/checksum against the registry entry. Only exact equality is a
hit.

This is correct for the existing verified shortpath contract. A
`jump-to-terminal` decision can skip downstream range execution and reuse an
existing terminal logits/token artifact. Hidden-state similarity alone does
not prove that downstream logits or sampled token are identical. A small
hidden-state delta can change the winning token when logits margin is small,
and that token error contaminates the rest of decode.

The existing `confidence_milli` field should be treated as artifact-source
confidence and candidate ranking metadata. It is not currently computed from
hidden-state similarity. Most runtime paths write fixed values such as `980`,
and CLI registration allows manual override. Therefore it should not be used
as if it were a mathematically derived match score.

## Related Code Locations

Memory Service types and exact lookup:

- `crates/sim-memory/src/lib.rs`
  - `HotTensorObjectRef`
  - `BoundaryTensorFingerprint`
  - `ExecutionArtifactObject`
  - `BoundaryLookupRequest`
  - `BoundaryLookupResponse`
  - `ShortpathSupportRecord`
  - `ShortpathDecisionRecord`
  - `LingquMemoryService::boundary_lookup`
  - `LingquMemoryDurableStore::write_block_payload`
  - `LingquMemoryDurableStore::read_block_payload`

Object Service payload access:

- `crates/sim-services/src/lib.rs`
  - `LingquObjectServiceStub`
  - `LingquObjectServiceStub::latest_record`
  - `LingquObjectServiceStub::get_copy`
  - `LingquObjectServiceStub::get_ref`
  - `LingquObjectServiceStub::export_snapshot`
  - `LingquObjectServiceStub::import_snapshot`

W5 Memory Service CLI and planner wiring:

- `crates/sim-cli/src/main.rs`
  - `run_w5_memory_boundary_lookup_request_with_registry_requirement`
  - `w5_plan_shortpath_decision_from_memory_support`
  - `validate_w5_shortpath_artifact_contract`
  - `w5_memory_boundary_registry_payload_from_refs`
  - `w5_memory_shortpath_stream_env_from_refs`
  - `build_w5_terminal_logits_payload`
  - `find_w5_terminal_logits_observation`
  - `w5_boundary_observations_from_summary`
  - `w5_boundary_fingerprints_from_summary`

W5 runtime artifact registration and ObjectRef conversion:

- `crates/sim-uapi/src/lib.rs`
  - `qwen3_hot_ref_from_obmm`
  - `qwen3_enqueue_w5_memory_runtime_commit`
  - `qwen3_commit_w5_memory_runtime_artifacts`
  - `qwen3_register_w5_runtime_terminal_support_artifacts`
  - `qwen3_object_registry_get_from_dir`
  - `qwen3_dense_reference_f32_payload_bytes`
  - `qwen3_w5_terminal_logits_payload`

Guest boundary registry and commit path:

- `guest-linux/aarch64/apps/w4_guest/w4_guest.c`
  - `qwen3_memory_shortpath_stream_entry_for_boundary`
  - `qwen3_memory_shortpath_validate_stream_boundary_fingerprint`
  - `qwen3_memory_shortpath_validate_single_boundary_fingerprint`
  - `qwen3_w5_memory_service_lookup_boundary`
  - `qwen3_boundary_controller_resolve_work_item`
  - `qwen3_read_object_ref_payload`
  - `qwen3_w5_memory_shortpath_commit` log site with `publish_hidden=0`

- `guest-linux/aarch64/w4_kvcache_db_service.c`
  - runtime range output/input ObjectRef publication for
    `hidden/<model>/node*/range-runtime-output|input/decode-step*`

## Goal

Add an approximate hidden-state match path without weakening the existing
correctness-preserving exact path.

The design target is:

```text
range exit hidden
  -> exact boundary lookup
  -> exact hit: verified shortpath may execute
  -> exact miss: optional approximate lookup
  -> approximate hit: require verify or guarded speculative jump
  -> approximate miss: continue
```

The user-visible outcome should be:

- exact matches remain deterministic and safe;
- approximate matches can improve latency only when explicitly enabled;
- approximate decisions are auditable and distinguishable from exact hits;
- terminal jumps are protected by verification and logits-margin guards.

## Design Principles

1. Keep the current exact path as the primary correctness path.
2. Approximate matching must be opt-in and separately reported.
3. A similarity score is not artifact provenance. Do not overload
   `confidence_milli` with match semantics.
4. The planner must know whether a decision came from exact fingerprint match,
   approximate hidden similarity, or a verified replay artifact.
5. Terminal jumps need stronger guards than layer jumps because they commit a
   token directly.

## Data Model Changes

Introduce explicit match metadata instead of reusing `confidence_milli`:

```text
source_confidence_milli
  How trustworthy the stored artifact is, based on provenance, verification,
  expiry, and registration policy.

match_score_milli
  Similarity score between the query output hidden and the candidate boundary
  hidden. Exact checksum match can be represented as 1000.

decision_confidence_milli
  Planner-level combined score after applying source confidence, match score,
  artifact kind, action type, logits margin, and verification policy.
```

Migration can be incremental:

- keep `ExecutionArtifactObject.confidence_milli` as the existing source
  confidence field for compatibility;
- add `match_score_milli` to shortpath support/decision records when the
  approximate path lands;
- later rename or alias `confidence_milli` to `source_confidence_milli` in a
  schema-versioned migration.

The exact path should continue to produce `match_score_milli=1000` if the new
field exists.

Terminal guard metadata should become first-class. Today sampled token,
runner-up token, `margin_milli`, logits checksum, text checksum, and candidate
metadata exist in W5 terminal logits payloads and guest log records, but they
are not first-class fields on `ExecutionArtifactObject`. The approximate
terminal path must either:

- add a small `TerminalLogitsMetadata` field to the logits artifact schema; or
- add a sidecar manifest keyed by `artifact_id`.

The first implementation should prefer a schema field because lookup policy
needs this metadata without parsing opaque terminal payloads in the hot path.

## Match Algorithm

The first implementation should use exact boundary identity followed by vector
similarity over hidden payloads.

Candidate prefilter:

1. `artifact.state == Verified`.
2. `artifact.model == request.model`.
3. `artifact.producer_boundary == request.boundary`.
4. `artifact.kind` is allowed by `allowed_actions`.
5. `artifact.confidence_milli >= min_source_confidence_milli`.
6. Optional `engram_state_id` binding matches.
7. Candidate payload and query hidden payload are available and shape-compatible.

Similarity computation:

1. Decode the query hidden and candidate boundary hidden payloads into the
   configured numeric dtype.
2. Compute cosine similarity as the default score:

   ```text
   cosine = dot(query, candidate) / (norm(query) * norm(candidate))
   match_score_milli = clamp(round(cosine * 1000), 0, 1000)
   ```

3. Add L2 distance as a secondary diagnostic metric:

   ```text
   normalized_l2 = l2(query - candidate) / max(l2(query), epsilon)
   ```

4. Reject NaN, zero-norm, dtype mismatch, shape mismatch, truncated payload,
   and payload checksum mismatch against the candidate object reference.

Threshold policy:

- exact hit: checksum/dtype/shape equality, no approximate math needed;
- approximate layer jump: require `match_score_milli >= layer_threshold`;
- approximate terminal jump: require both
  `match_score_milli >= terminal_threshold` and sufficient terminal logits
  margin;
- default thresholds should be conservative and disabled unless explicitly
  configured.

Initial experimental thresholds:

- approximate mode default: disabled;
- approximate layer threshold when enabled: `995`;
- approximate terminal threshold when enabled: `999`;
- approximate terminal margin threshold: `1000`;
- approximate terminal commit: disabled by default even above threshold; emit
  `RequireVerify` unless an explicit experiment flag enables guarded commit.

These values are deliberately conservative. They are not a quality claim; they
are starting points for synthetic validation and should be tuned only after
exact-regression and low-margin-rejection tests pass.

## Payload Access And Dtype Policy

Existing payload mechanisms:

- `LingquMemoryDurableStore::read_block_payload` reads durable
  `LingquBlockPayloadRef` bytes and validates block ref metadata.
- `LingquObjectServiceStub::get_copy` and `get_ref` read committed hot object
  payloads from an imported Object Service snapshot or in-memory OBMM pool.
- `qwen3_object_registry_get_from_dir` reads W5 object-registry payloads by
  `LingquObmmObjectRefWire` and validates bytes/checksum.
- The guest helper `qwen3_read_object_ref_payload` reads through Object
  Service first and then the registry fallback.

Current W5 hidden ObjectRefs are recorded as `TensorDType::Opaque` with shape
`[payload_bytes]` when converted by `qwen3_hot_ref_from_obmm`. That is enough
for exact checksum matching, but not enough for a generic numeric similarity
engine.

Initial implementation choice:

- implement the similarity engine for `TensorDType::F32` only;
- decode little-endian `f32` directly with the standard library;
- do not add `ndarray`, `half`, BLAS, GEMM, or ANN dependencies;
- reject `Opaque`, `U8`, `U32`, and `U64` for approximate scoring until an
  explicit profile adapter converts them into typed numeric vectors;
- keep the scorer in `sim-memory`, because boundary lookup and artifact
  policy live there and `sim-memory` must not depend on W5-specific `sim-uapi`
  profile code.

W5-specific `Opaque` support should be a separate adapter step:

1. `sim-uapi` or the CLI exports a typed F32 hidden sidecar artifact for the
   same boundary, or writes a typed `HotTensorObjectRef`.
2. Memory Service approximate lookup scores only that typed sidecar.
3. The original opaque W5 hidden artifact continues to back exact checksum
   matching and guest registry compatibility.

This keeps generic Memory Service logic free of W5 payload-layout assumptions.

## Shortpath Decision Semantics

The existing seven implementation rules are:

1. Preserve the exact path: if `bytes/checksum/dtype/shape` match, return the
   current verified shortpath support.
2. Add the approximate path only after exact miss.
3. Approximate candidates must still pass model, boundary identity,
   allowed-action, verified-state, source-confidence, and optional Engram-state
   filters.
4. Similarity must read actual query and candidate hidden payloads and compute
   an explicit score such as cosine similarity or normalized L2.
5. Approximate matches must produce `RequireVerify` or a jump with
   `verify_required=true` until a verification path proves the artifact is
   safe for direct execution.
6. Terminal logits reuse needs an extra guard: the candidate terminal logits
   margin must be above a configured threshold. Low-margin terminal artifacts
   must fall back to `Continue` or `RequireVerify`.
7. CLI and tests must cover exact hit, approximate hit, approximate miss,
   low-margin reject, and missing-payload reject.

The planner must never silently convert an approximate hit into an ordinary
verified hit. Audit logs and run summaries need distinct reasons, for example:

```text
verified_execution_artifact_support
approximate_hidden_match_requires_verify
approximate_hidden_match_low_terminal_margin
approximate_hidden_match_payload_unavailable
```

## Implementation Steps

### Step 1: Schema And CLI Contract

Add new fields behind schema-version-compatible defaults:

- `match_score_milli`
- `match_metric`
- `match_mode`: `exact` or `approximate`
- optional `normalized_l2_milli`
- optional terminal margin fields used by the guard

Add CLI flags:

- `--shortpath-match-mode=exact|approximate|exact-then-approximate`
- `--min-source-confidence-milli`
- `--min-match-score-milli`
- `--min-terminal-margin-milli`
- `--approximate-requires-verify`

Keep existing `--min-confidence-milli` as a compatibility alias, but document
that it currently maps to source confidence.

### Step 2: Payload Access

Add a Memory Service helper that resolves the query hidden payload and
candidate boundary hidden payload from existing storage mechanisms:

- hot ObjectRef path: use an Object Service snapshot and
  `LingquObjectServiceStub::get_copy` or `get_ref`;
- durable path: use `LingquMemoryDurableStore::read_block_payload`;
- W5 registry path: keep in `sim-uapi`/CLI adapter code and convert into a
  typed artifact before Memory Service scoring.

Requirements:

- verify object version, bytes, checksum, dtype, and shape before scoring;
- reject opaque W5 payloads unless a profile-specific adapter has already
  produced typed F32 hidden bytes;
- return a structured miss reason instead of panicking or silently continuing.

### Step 3: Similarity Engine

Implement a small deterministic CPU similarity engine in `sim-memory`:

- cosine similarity;
- normalized L2;
- `TensorDType::F32` little-endian decoding only in the first pass;
- explicit rejection for unsupported dtype or mismatched shape.

Current `TensorDType` supports `U8`, `U32`, `U64`, `F32`, and `Opaque`. There
is no `BF16` or `F16` variant today. Adding bf16/fp16 support is a separate
schema and decoder extension, not part of the first implementation.

Do not introduce ANN indexing in the first pass. The candidate set is already
narrowed by exact boundary identity, so the first implementation should be
simple and auditable.

### Step 4: Boundary Lookup Policy

Refactor `boundary_lookup` into two stages:

```text
exact_boundary_lookup()
approximate_boundary_lookup()
```

Default behavior remains exact-only. Approximate lookup runs only when the
request or service policy enables it.

Exact support preserves the existing action. Approximate support returns:

- `RequireVerify` by default;
- optionally `JumpToLayer` or `JumpToTerminal` with `verify_required=true` if
  the downstream execution path can enforce verification before commit.

### Step 5: Terminal Guard

For `JumpToTerminal`, require terminal logits metadata to expose enough
evidence:

- sampled token;
- runner-up token;
- `margin_milli`;
- logits checksum;
- candidate count or full-vocab check status.

This metadata already exists in W5 terminal logits payloads and guest logs, but
not as direct fields on `ExecutionArtifactObject`. Step 1 must make it
available to Memory Service lookup through `TerminalLogitsMetadata` or an
artifact sidecar before Step 5 can be enforced without payload parsing.

If `margin_milli < min_terminal_margin_milli`, reject the approximate terminal
jump. This should be a normal miss/continue path, not a hard runtime failure.

### Step 6: Guest And Registry Reporting

Keep the guest registry exact path unchanged at first.

When approximate decisions are exported to W5:

- include `match_mode`, `match_score_milli`, and `verify_required` in compact
  stream or boundary registry metadata;
- make run logs distinguish exact and approximate hits;
- prevent `publish_hidden=0` terminal commit unless the decision passed the
  configured verification guard.

### Step 7: Rollout

Roll out in this order:

1. schema and CLI fields with exact-only behavior;
2. tests for source-confidence versus match-score semantics;
3. approximate lookup behind disabled-by-default flag;
4. layer-jump approximate `RequireVerify`;
5. terminal approximate path with margin guard;
6. guest registry export/import support;
7. performance validation after correctness gates pass.

## Verification Requirements

Every implementation PR must include CLI coverage and automated tests.

Required unit tests:

- exact fingerprint match returns verified support with `match_score_milli=1000`;
- exact fingerprint mismatch does not hit when approximate mode is disabled;
- approximate mode computes cosine score and returns `RequireVerify` above
  threshold;
- approximate mode returns `Continue` below threshold;
- approximate terminal candidate with low `margin_milli` is rejected;
- missing query payload returns a structured miss reason;
- missing candidate payload returns a structured miss reason;
- dtype mismatch and shape mismatch are rejected;
- source confidence below threshold is rejected even when similarity is high;
- candidate ordering uses match score and source confidence explicitly.

Required CLI tests:

- build a boundary request from W5 summary and run exact-only lookup;
- register two candidate artifacts with different source confidence and match
  scores;
- run approximate lookup with threshold pass;
- run approximate lookup with threshold fail;
- verify list commands print match mode and match score.

Required guest/run-summary checks:

- exact hit still reports the existing hit path;
- approximate hit reports `match_mode=approximate`;
- approximate terminal low-margin path reports a guarded miss;
- `publish_hidden=0` only appears for decisions allowed to commit terminal
  output;
- `guest_worker_shortpath_summary` separates exact hits from approximate hits.

Required full validation before enabling by default:

- `cargo test --workspace`;
- targeted W5 Memory Service CLI tests;
- W5 guest shortpath exact regression run;
- W5 guest approximate shortpath run with synthetic high-similarity artifact;
- W5 guest approximate shortpath low-margin rejection run.

## Open Questions

1. Should approximate `JumpToTerminal` ever commit without recomputing logits,
   or should it always be speculative until a verification range confirms the
   token?
2. Should approximate matches be limited to same-run artifacts initially, or
   can cross-run artifacts participate once model/profile/tokenizer bindings
   match?
3. Should terminal metadata be embedded directly in `ExecutionArtifactObject`,
   or stored in a separate manifest keyed by `artifact_id`?

## Non-Goals

- Do not replace exact fingerprint hit semantics.
- Do not implement ANN/HNSW indexing in the first pass.
- Do not treat `confidence_milli` as a hidden similarity score.
- Do not allow approximate terminal commits without an explicit guard.
- Do not silently downgrade verification failures into successful hits.
