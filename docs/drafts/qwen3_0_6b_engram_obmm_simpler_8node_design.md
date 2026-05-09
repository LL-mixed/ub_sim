# Qwen3 0.6B Engram + OBMM Pool + Simpler 8-Node Design

## Goal

Build an 8-node simulation path where Qwen3 0.6B decode runs as a CPU+NPU
cooperative pipeline:

```text
CPU tokenizer / sampler / engram
  <-> OBMM shmem pool control and payload state
  <-> sim-uapi object service
  <-> sim-runtime / sim-chipbackend-simpler
  <-> simpler HostBuildGraph runtime
```

The goal is not to add a new Qwen3 network layer. `engram` is modeled as
decode-time state and policy: it observes generated token history, candidate
tokens, logits metadata, and optional memory hints, then constrains or selects
the next token. Qwen3 dense forward remains owned by the existing
`qwen3_dense_0_6b` pipeline and the `simpler_capi` chip backend.

The first end-to-end target command should remain user-facing and compact:

```bash
SIM_QWEN3_0_6B_WEIGHTS_PATH=/path/to/Qwen3-0.6B \
cargo run --release -p sim-cli -- qwen3-decode-loop \
  --scenario 8host \
  --max-token 128 \
  --prompt "Capital of China is" \
  --matmul-batch 4 \
  --engram \
  --no-repeat-ngram-size 3 \
  --repetition-penalty 1.05
```

## Non-Goals

- Do not make `engram` a required Qwen3 0.6B forward operator.
- Do not put the first engram implementation inside `simpler` kernels.
- Do not replace the existing Qwen3 layer partitioning, KV cache path, object
  service model, or HostBuildGraph artifact producer.
- Do not introduce a global OBMM MPSC queue. Keep the existing
  `queue[dst][src]` SPSC ownership model.
- Do not require guest-visible Qwen3 workload changes for the first simulator
  slice. Guest integration can follow after the host-side 8-node path is
  observable and deterministic enough.

## Existing Boundaries To Preserve

### Simpler Boundary

`simpler` is the chip backend execution boundary. For Qwen3 0.6B it should keep
owning numeric compute:

- matmul and batched matmul dispatch
- projection / attention / MLP tiles where the current runtime path already
  delegates to `simpler_capi`
- runtime artifact loading through the existing matmul/vector manifest flow

The simulator should not duplicate `simpler` internal task rings, scope tokens,
or AIC/AIV scheduling. The simulator owns orchestration, descriptors, object
identity, placement, tracing, and completion semantics around the chip backend.

### OBMM Pool Boundary

The OBMM pool remains an owner-sharded shared memory transport:

```text
queue[dst][src] = descriptors from src to dst
```

High-frequency metadata must remain single-writer:

- `tail` is written only by the producer node.
- `head` is written only by the destination node.
- producer-owned payload arenas are written only by the exporting node.
- descriptor payload references use `region_id + offset + len`, not process
  pointers.

For this design, OBMM pool carries state and payload references between CPU
orchestration and per-node chip backend work. It is not the model math engine.

### Object Service Boundary

The object service remains the logical namespace and placement owner for Qwen3
runtime data:

```text
qwen3/session/{session_id}/tokens/history
qwen3/session/{session_id}/engram/state/step/{step}
qwen3/session/{session_id}/logits/step/{step}/candidates
qwen3/session/{session_id}/kv/layer/{layer}/tile/{tile}/position/{pos}/k
qwen3/session/{session_id}/hidden/boundary/node/{src}/to/{dst}/step/{step}
```

OBMM pool is one payload backend for hot objects. DB metadata and object
versions decide what is ready and where it lives.

## Architecture

```text
+------------------------- host process / sim-cli --------------------------+
| tokenizer + chat template                                                  |
| decode loop controller                                                     |
| engram policy and sampler                                                   |
+-----------------------------+---------------------------------------------+
                              |
                              v
+------------------------- sim-uapi / object service ------------------------+
| request/session record                                                     |
| token history object                                                       |
| logits/candidate object                                                    |
| engram state object                                                        |
| KV and hidden tensor object refs                                           |
+-----------------------------+---------------------------------------------+
                              |
                              v
+---------------------- OBMM shmem pool model -------------------------------+
| per-node exported region                                                   |
| queue[dst][src] descriptors                                                |
| producer-owned TX arenas                                                   |
| visibility, ordering, backpressure counters                                |
+-----------------------------+---------------------------------------------+
                              |
                              v
+------------------- sim-runtime + simpler_capi backend ---------------------+
| HostBuildGraph manifest loading                                            |
| Qwen3 matmul / vector / runtime dispatch                                   |
| completion and fault propagation                                           |
+---------------------------------------------------------------------------+
```

The CPU side makes token decisions. The NPU side computes Qwen3 dense forward.
OBMM pool and object service make each cross-boundary state transition explicit
and testable.

## Decode Step Flow

### Prefill

1. CLI renders the Qwen3 chat template unless `--raw-prompt` is set.
2. Tokenizer encodes ordinary text and Qwen special tokens.
3. `sim-uapi` creates a session object:
   - token history
   - prompt token buffer
   - empty engram state
   - KV cache namespace
4. The 8-node topology partitions Qwen3 layers across nodes.
5. Node-local Qwen3 work dispatches through `sim-chipbackend-simpler`.
6. Hidden boundaries, KV refs, and prefill logits/candidates are published as
   object records. Hot payloads use OBMM pool placement.
7. CPU engram consumes prompt token history and prefill candidates, then writes
   the first selected token object.

### Incremental Decode

For each generated token:

```text
1. CPU controller resolves token history and previous engram state.
2. CPU controller dispatches next Qwen3 decode step to node range owners.
3. simpler computes layer tiles and logits/candidates.
4. sim-uapi publishes logits/candidate metadata through the object service.
5. Hot candidate/logits payload refs are carried through OBMM descriptors.
6. CPU engram reads candidate metadata plus token history.
7. CPU engram applies constraints and chooses next token.
8. sim-uapi appends next token to token history and updates engram state.
9. Loop stops on Qwen stop token or max token.
```

The important property is that the feedback edge is explicit:

```text
NPU logits/candidates -> OBMM/object service -> CPU engram -> next token -> NPU
```

## Engram Model

The first implementation should define `EngramState` as a small deterministic
decode-side record:

```rust
pub struct Qwen3EngramState {
    pub session_id: u64,
    pub step_index: u64,
    pub token_count: u64,
    pub rolling_hash: u64,
    pub ngram_window: u8,
    pub repetition_penalty_milli: u32,
    pub blocked_token_count: u32,
    pub selected_token: u64,
    pub state_checksum: u64,
}
```

MVP behavior:

- `--no-repeat-ngram-size N`: reject candidates that would create a repeated
  N-gram in the generated history.
- `--repetition-penalty P`: down-rank repeated tokens or reject them when the
  available candidate set is large enough.
- `--engram-history-window W`: cap history scan cost for long generations.
- deterministic tie-breaking based on step, token id, and existing temperature
  sampling seed.

The first implementation can operate on token ids and candidate rank metadata.
It does not need full-vocab logits mutation in the hot path. If full-vocab
logits are available, the same policy can later be promoted into a
logits-processor phase.

## OBMM Payloads And Descriptors

Use three object/payload classes:

### Token History

Small and append-heavy. Store as an inline object for unit tests and as an OBMM
producer-owned payload for the 8-node path:

```text
kind: TokenBuffer
key: qwen3/session/{session_id}/tokens/history
payload: u64 token ids
placement: Inline for local tests, OBMM for 8-node validation
```

### Candidate Metadata

Produced after each Qwen3 forward step:

```text
struct Qwen3CandidateRecord {
    uint64_t step_index;
    uint64_t rank;
    uint64_t token_id;
    int32_t logit_milli;
    int32_t adjusted_score_milli;
    uint64_t token_piece_checksum;
};
```

For MVP this can carry the current selected token plus runner-up and a bounded
top-K candidate set. Full-vocab logits should remain optional because moving
151,936 logits through OBMM every token is expensive and unnecessary for
no-repeat-ngram validation.

### Engram State

Produced by CPU after candidate selection:

```text
kind: Metadata
key: qwen3/session/{session_id}/engram/state/step/{step}
payload: Qwen3EngramState
placement: Inline or OBMM control payload
```

Each update is versioned. Consumers should resolve the exact step version, not
latest, when constructing the next decode input.

## 8-Node Placement

Use `scenarios/mvp_8host_single_domain.yaml` through `--scenario 8host`.

Recommended partitioning:

- Qwen3 layer work remains balanced by existing node range ownership.
- Node 0 owns session control and CPU engram for MVP.
- Nodes 1-7 run Qwen layer partitions and publish boundary objects.
- Every node exports an OBMM region with:
  - ingress queues from all peers
  - producer-owned TX arena
  - optional engram/candidate control slab

This keeps the first design simple: engram is centralized on node 0. A later
phase can shard engram state, but that should only happen after one-node CPU
engram proves correctness and observability.

## CLI

Extend `sim-cli qwen3-decode-loop`:

```text
--engram
--engram-mode cpu
--engram-pool obmm
--no-repeat-ngram-size N
--repetition-penalty P
--engram-history-window W
--engram-report none|summary|steps|verbose
```

Defaults:

- `--engram` defaults to off.
- `--engram-mode cpu` is the only MVP mode.
- `--engram-pool obmm` requires `--scenario 8host` for the 8-node validation
  target, but unit tests can use inline/local placement.
- `--no-repeat-ngram-size 0` disables n-gram blocking.
- `--repetition-penalty 1.0` disables penalty.

The CLI should fail with an actionable error if `--engram-pool obmm` is used
without an OBMM-capable scenario or if the selected report mode requires
candidate data that was not produced.

## Reports And Metrics

Add engram fields to Qwen3 decode reports:

```text
engram_enabled
engram_mode
engram_pool
engram_history_tokens
engram_candidate_count
engram_blocked_token_count
engram_selected_token
engram_state_checksum
engram_policy_checksum
```

Add OBMM/object-service counters already visible in verbose reports:

```text
obmm_pool_enabled
obmm_pool_payload_write_count
obmm_pool_payload_read_count
obmm_pool_queue_submit_count
obmm_pool_queue_deliver_count
obmm_pool_bytes_used
```

The eight-node validation should prove:

- engram state is updated once per decode step;
- every selected token has a matching candidate record;
- token history length equals prompt tokens plus generated tokens;
- no rejected N-gram appears in generated token history when
  `--no-repeat-ngram-size > 0`;
- OBMM queue submit/deliver counts advance when `--engram-pool obmm` is used;
- simpler dispatch still runs Qwen forward and is not bypassed by engram.

## Implementation Plan

### Phase 1: CPU Engram Without OBMM

Purpose: prove policy and CLI semantics without transport complexity.

Work:

- Add `Qwen3EngramConfig` and `Qwen3EngramState` to `sim-uapi`.
- Add CLI flags and parser tests in `sim-cli`.
- Integrate engram into the token selection step after candidate parsing.
- Add unit tests for:
  - no-repeat-ngram blocking;
  - repetition penalty tie-breaking;
  - stop token handling remains higher priority than engram;
  - default behavior is unchanged when `--engram` is absent.

### Phase 2: Object Service Placement

Purpose: make engram state and token history observable as logical objects.

Work:

- Add object keys for token history, candidate metadata, and engram state.
- Publish one engram state object per decode step.
- Extend decode report with engram object counts and checksums.
- Add tests that resolve exact step versions and verify checksums.

### Phase 3: OBMM Pool Transport

Purpose: move hot token/candidate/engram payloads through the OBMM pool model.

Work:

- Add OBMM placement mode for token history and engram state.
- Add fixed-size candidate descriptors.
- Reuse existing owner-sharded `queue[dst][src]` model.
- Add backpressure behavior for full queues: return retryable error, poll, and
  retry within a bounded budget.
- Add tests for:
  - local inline placement parity with OBMM placement;
  - queue submit/deliver count per decode step;
  - payload checksum after remote read;
  - slot reuse under pressure.

### Phase 4: 8-Node Simpler Path

Purpose: validate the target integrated run.

Work:

- Require `--scenario 8host`.
- Ensure `host_matmul` and batched matmul manifests are prepared.
- Run Qwen3 decode with `simpler_capi` and `--matmul-batch 4`.
- Confirm engram report and OBMM report counters advance.
- Add a host-side validation command or script:

```bash
SIM_QWEN3_0_6B_WEIGHTS_PATH=/path/to/Qwen3-0.6B \
cargo run --release -p sim-cli -- qwen3-decode-loop \
  --scenario 8host \
  --max-token 64 \
  --prompt "Hello. Qwen3." \
  --matmul-batch 4 \
  --engram \
  --engram-pool obmm \
  --no-repeat-ngram-size 3 \
  --temperature 0.6 \
  --decode-report steps
```

### Phase 5: Optional NPU-Side Engram Acceleration

Only consider this after Phase 4 proves correctness.

Candidate acceleration targets:

- candidate filtering for small top-K arrays;
- rolling hash update;
- repeated-token score adjustment.

Do not move full history scanning or object-service resolution into `simpler`
until profiling shows CPU engram is a real bottleneck.

## Failure Modes

- **False stop:** engram must not override Qwen stop token detection.
- **Dead queue:** OBMM queue full must be visible as retryable backpressure, not
  a silent dropped descriptor.
- **Stale state:** next decode step must resolve the exact previous engram step
  version.
- **Payload visibility:** producer-owned OBMM payload writes must be visible
  before descriptor publication.
- **Synthetic bypass:** tests must prove Qwen forward still uses the real
  `simpler_capi` path when requested.
- **Prompt pollution:** special tokens used for chat template must stay
  tokenized as special tokens, not ordinary text.

## Open Questions

1. Is `engram` intended to mean only no-repeat-ngram/repetition control, or does
   it also include memory retrieval and prompt augmentation?
2. Should candidate metadata be top-K only for MVP, or should full-vocab logits
   be available behind a report/debug flag?
3. In guest validation, should CPU engram run in the host simulator process or
   in a guest userspace service that uses OBMM directly?
4. What is the target maximum token rate where CPU engram becomes too slow and
   NPU-side acceleration becomes worth considering?

## Acceptance Criteria

- `cargo test --workspace` passes.
- `sim-cli qwen3-decode-loop --engram` has parser and policy tests.
- With `--engram` absent, existing Qwen3 decode reports remain behaviorally
  compatible.
- With `--no-repeat-ngram-size 3`, generated token history contains no repeated
  3-gram after engram starts enforcing.
- With `--engram-pool obmm`, object-service reports show engram/token/candidate
  objects and OBMM pool counters.
- `--scenario 8host --matmul-batch 4 --engram --engram-pool obmm` runs through
  the `simpler_capi` Qwen3 path and prints timing plus engram summary.
