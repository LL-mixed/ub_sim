# Paper-Aligned Engram Design And Migration Plan

## Goal

Align the simulator Engram work with `docs/Engram_paper.pdf`:

```text
raw token ids
  -> tokenizer compression P: V -> V'
  -> canonical suffix ngrams, primarily {2,3}-gram
  -> per-order multi-head hash
  -> trained Engram embedding table lookup
  -> context-aware gate and residual hidden-state injection
  -> Attention / MoE / downstream Transformer layers
```

The target is not just a decode-time repetition policy. Engram should become a
paper-compatible conditional memory module whose table can be produced by
post-training or fine-tuning, stored by Lingqu Memory Service, and consumed by
W5 cluster inference.

## Current Terminology Reset

Current repo usage is overloaded:

1. **Engram decode policy**
   - Implemented in `crates/sim-cli/src/main.rs` and
     `guest-linux/aarch64/w4_guest_qemu_demo.c`.
   - Applies no-repeat ngram and repetition penalty during candidate token
     selection.
   - Does not implement paper Engram.
2. **Engram context op**
   - Implemented by `crates/sim-models/src/engram_context.rs` and the W5
     `simpler-host` path.
   - Implements the latter half of paper Engram for already-materialized
     indices:
     `table[indices] -> mean -> sigmoid gate -> residual`.
   - Does not generate canonical ngram indices and does not own table training.
3. **Paper Engram**
   - Conditional memory module inserted into Transformer hidden-state layers.
   - Uses tokenizer compression, canonical ngrams, multi-head hashing, trained
     tables, contextualized gating, and residual hidden-state injection.

Going forward:

- Use **paper Engram** for the hidden-state conditional memory module.
- Use **decode policy** for no-repeat/repetition token selection.
- Use **Engram context op** for the executable gather/gate/residual operator
  backend used by paper Engram.

### 第一批准备状态（2026-05-27）

- [x] 将现有术语边界固定为三段：`decode policy` / `engram context` / `paper Engram`。
- [x] 为 no-repeat 路径加上 exact-key 索引（已使用 projection 后 token，并有单测覆盖）。
- [x] 在报告层新增 `decode_policy_*` 前缀视图，保留 `engram_*` 兼容项，避免历史脚本受影响。
- [x] 明确当前可承诺行为：当前已实现仅覆盖 decode policy（采样约束）与 context-op 后半段。
- [x] 补齐多阶/多头 canonical ngram 索引描述与 runtime 可消费元数据。
- [x] 为 Memory Service 增加 paper Engram projection/hash-config 一等 artifact 与 CLI 注册入口。
- [x] 为 Engram context-op 增加 paper-style multi-order/multi-head CPU reference 与 CLI fixture。
- [x] 为 Memory Service 增加 table/gate/module manifest 到 runtime operand bundle 的解析与 CLI 验证入口。
- [x] 为 UAPI/W5 `ENGRAM_STATE` 增加 paper manifest v2，可在不改变 guest object-ref 搬运语义的前提下消费 multi-order/multi-head table/gate refs。
- [x] 增加 `publish-paper-engram-state-ref`，可将 Memory Service runtime operand bundle 发布成 UAPI paper `ENGRAM_STATE` manifest 和 W5 Object Service snapshot。
- [x] 在 UAPI/W5 range forward reference path 中让 paper `ENGRAM_STATE` 按 manifest layer boundary 注入；legacy `ENGRAM_STATE` 继续 terminal-only。
- [x] 将 layer-boundary paper injection 接入 W5 cluster decode 脚本入口，并在 runtime context report 中输出 node/layer/step；explicit `SIM_QWEN3_GUEST_ENGRAM_STATE_REF` 入口需要使用 `*_engram_decode` profile，且不能与 Memory Service bootstrap/reuse 混用。
- [x] 使用 W5 cluster decode artifact bundle 做端到端验证，确认 paper layer-boundary injection 在真实 decode run 中命中。
- [x] 规划 paper Engram 训练产物生成流程与质量声明验证；Memory Service 已有 `training_recipe` / `eval_report` manifest、CLI 注册入口与质量声明校验。

## Design Objectives

1. Support paper-compatible Engram table construction during post-training or
   fine-tuning.
2. Extend Lingqu Memory Service so it can store, version, validate, and serve
   Engram tables and tokenizer-compression metadata.
3. Support generating Engram tables from business continued-pretraining or SFT
   data.
4. Rebase the current W5 Engram work onto the paper-compatible architecture:
   current no-repeat policy becomes a sampler constraint, while current
   context-op paths become Engram operator backends.

## Paper Requirements To Implement

### Tokenizer Compression

Add an explicit model-bound projection:

```text
P_model: raw_token_id -> canonical_token_id
```

Requirements:

1. Build `P_model` from the target tokenizer vocabulary.
2. Normalize token text using the paper-compatible policy:
   - NFKC normalization
   - lowercasing
   - tokenizer-specific handling of space markers such as `Ġ`
   - stable handling of special tokens
3. Persist the projection as an immutable model artifact.
4. Report compression ratio and collision classes.
5. Use canonical IDs for paper Engram ngram keys. Raw token IDs may still be
   reported for debugging and sampler policy.

Special tokens must not be merged with ordinary text tokens unless the
training pipeline explicitly declares that behavior. A bad projection corrupts
both training and inference.

### Canonical Ngram Indexing

For each position `t`, generate canonical suffix ngrams:

```text
g(t, n) = (x'_{t-n+1}, ..., x'_t)
```

Initial orders:

```text
orders = {2, 3}
```

The runtime must maintain rolling canonical token history so index generation
is incremental. For decode, each new token updates only the affected suffix
keys. For training, the dataloader can precompute keys per microbatch or build
them on device/host as part of input preparation.

### Multi-Head Hashing

For each order `n`, use `K` deterministic hash heads:

```text
row = hash(order=n, head=k, canonical_ngram) % table_rows(order=n, head=k)
```

Requirements:

1. Hash implementation must be shared by training, CPU reference, guest/W5
   runtime, and fused backends.
2. Hash config is part of the model artifact:
   - orders
   - heads per order
   - table rows per order/head
   - seed/primes/version
   - canonical projection checksum
3. Hash collision is acceptable for Engram table lookup because it is part of
   the learned parameterization. It is not acceptable for correctness policies
   such as no-repeat; those need exact-key/tag validation.

The vendor exploration already has a candidate shape in
`vendor/pto-isa/kernels/manual/a5/engram_simt/engram_common.h`, but it is not
yet the repo-wide canonical hash contract.

### Trained Engram Tables

Engram tables are model parameters, not RAG-style document embeddings.

Training pipeline:

```text
base checkpoint
  -> insert Engram module at selected layers
  -> initialize Engram tables and gate/fusion parameters
  -> train on continued-pretraining or SFT data
  -> update activated table rows through LM loss
  -> export Engram artifact bundle
```

Recommended first training mode:

1. Freeze base model.
2. Train Engram tables and gate/fusion parameters.
3. Optionally train LoRA adapters after the Engram-only run proves benefit.
4. Use business data for domain adaptation, but keep table size bounded to
   avoid overfitting small datasets.

Without trained tables, paper Engram can still validate system behavior and
performance using zero/random/fixture tables, but it must not claim model
quality improvement.

### Hidden-State Injection

Paper Engram must operate before logits, inside the model:

```text
H(l)
  -> Engram lookup and gate
  -> H(l) + Y
  -> subsequent Transformer computation
```

This differs from the current decode policy:

```text
logits/candidates -> no-repeat/repetition policy -> selected token
```

W5 should expose insertion points tied to layer boundaries. Initial placement
should target early layers, consistent with the paper's finding that Engram
offloads local/static pattern reconstruction from shallow Transformer blocks.

## Memory Service Requirements

Lingqu Memory Service must treat Engram as a first-class model artifact family.

### Artifact Types

Add semantic object families:

```text
/lingqu/memory/models/<model-id>/engram/projection/<version>.json
/lingqu/memory/models/<model-id>/engram/hash-config/<version>.json
/lingqu/memory/models/<model-id>/engram/table/<layer>/<order>/<head>/manifest.json
/lingqu/memory/models/<model-id>/engram/table/<layer>/<order>/<head>/block-*.bin
/lingqu/memory/models/<model-id>/engram/gate/<layer>/manifest.json
/lingqu/memory/models/<model-id>/engram/module/<engram-id>.json
```

The exact durable path can evolve with the durable DFS/Block backend, but the
semantic split must remain stable:

1. tokenizer projection
2. hash/index config
3. table payloads
4. gate/fusion weights
5. module manifest binding all of the above to a model checkpoint

### Engram Module Manifest

Minimum manifest fields:

```text
engram_id
base_model_id
base_checkpoint_checksum
tokenizer_id
tokenizer_projection_ref
hash_config_ref
layers[]
orders[]
heads_per_order
hidden_size
memory_dim
table_dtype
table_layout
gate_kind
training_recipe_ref
quality_claim
payload_checksums
```

`quality_claim` must distinguish:

- `none`: zero/random/fixture tables, system validation only.
- `posttrain`: trained on continued-pretraining data.
- `finetune`: trained on SFT/business data.
- `imported`: externally produced table, provenance recorded.

### Runtime Serving

Memory Service should support:

1. Resolve Engram module by `(model_id, engram_id)`.
2. Produce canonical projection and hash config for runtime.
3. Resolve table row blocks by `(layer, order, head, row_range)`.
4. Build prefetch plans from deterministic future token/ngram indices when
   decode history is known.
5. Emit ObjectRefs for W5 runtime operands.
6. Rebuild indexes from durable manifests after restart.

This aligns with the SIM_DEC/OBMM direction: Engram table bytes may live in
host DRAM, Object Service, block-backed durable storage, or imported memory,
but Memory Service owns semantic identity and placement decisions.

## W5 Rebase Plan

### What To Keep

Keep these current pieces as useful scaffolding:

1. Object Service transport for Engram-related state.
2. `EngramStateObjectRef` style object references.
3. `crates/sim-models/src/engram_context.rs` as CPU reference for
   gather/gate/residual, after extending it to accept paper-compatible
   multi-order/multi-head descriptors.
4. `simpler-host` context-op backend as a semantic backend.
5. Vendor fused SIMT exploration under
   `vendor/pto-isa/kernels/manual/a5/engram_simt/`.

### What To Reclassify

The current no-repeat/repetition implementation should be renamed in docs and
interfaces as decode policy:

```text
qwen3 decode policy:
  no_repeat_ngram
  repetition_penalty
  blocked_token_ids
```

It may remain enabled for practical generation quality, but it is not the paper
Engram module. Its rolling ngram index can reuse tokenizer compression and
canonical IDs, but it must keep exact-key validation instead of relying on
lossy hash table rows.

### What To Add

W5 needs a paper Engram runtime path:

```text
range/layer boundary hidden
  -> canonical ngram indices for current positions
  -> Memory Service resolves/prefetches table rows
  -> Engram context op backend computes augmented hidden
  -> downstream Qwen layer execution continues
```

Current implementation status:

- `sim-memory` can resolve paper Engram module/projection/hash/table/gate manifests into layer-scoped runtime operands.
- `sim-uapi` can consume a v2 `ENGRAM_STATE` manifest that points to paper table/gate objects and run the multi-order/multi-head CPU reference path.
- `sim-cli lingqu-memory publish-paper-engram-state-ref` can publish Memory Service runtime operands into that UAPI manifest and export a W5 Object Service snapshot.
- W5 range forward can invoke the context op at manifest-configured layer boundaries. A real Qwen3-14B 1-step W5 cluster decode run passed with an explicit paper artifact bundle (`run_id=2026-05-27_15-40-40_w5_qwen3_14b_engram_decode_17287`), and nodeA reported `qwen3-engram-context` at `layers=[0,5)` with `mode=cpu-reference-paper-object-ref`.
- `sim-memory` now treats paper Engram `training_recipe` and `eval_report` as durable artifacts. `Posttrain` and `Finetune` module quality claims must bind matching recipe and eval evidence before registration or validation succeeds.
- `sim-cli lingqu-memory` now supports `register-paper-engram-training-recipe`, `register-paper-engram-eval-report`, and `validate-paper-engram-quality`. Fixture-generated tables still use `quality_claim=none`.
- Paper Engram eval reports now enforce no-regression against the base-model validation loss and, when present, the decode-policy-only validation loss before a trained quality claim can pass.
- `quality_claim=imported` now requires provenance evidence too: the module must bind an `ExternalImport` training recipe plus an eval report, so externally produced tables cannot bypass the same Memory Service quality gate.
- Trained/imported quality claims now require runtime acceptance evidence in the eval report: all four Phase 6 loss variants (`base`, `base+decode_policy`, `base+paper_engram`, `base+paper_engram+decode_policy`), CPU/backend output match, non-zero-table hidden/output checksum deltas, row-prefetch locality counters, and bounded backend latency.
- `sim-cli lingqu-memory build-paper-engram-eval-report-from-w5-summary` can now generate an eval report manifest from W5 paper Engram runtime logs, including context output checksum evidence, zero-table comparison evidence, row-prefetch counters, backend latency, and terminal output checksum aggregation.
- Memory Service can resolve paper Engram table row block refs by `module_id/layer/order/head/row_range`; `sim-cli lingqu-memory resolve-paper-engram-table-row-blocks` exposes this as the row-block ObjectRef materialization base for runtime prefetch and Object Service publication.
- Memory Service can build deterministic paper Engram row prefetch plans from canonical token history using the shared `sim-models::engram_hash` implementation; `sim-cli lingqu-memory plan-paper-engram-row-prefetch` exposes the planned row refs and backing block refs, and `publish-paper-engram-row-prefetch` publishes the row plan as Object Service metadata. W5 entrypoints now carry the published plan through `SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF` / `--engram-row-prefetch-ref` alongside the required paper `ENGRAM_STATE` ref.
- W5 paper `ENGRAM_STATE` publication now emits a v3 manifest that carries tokenizer-projection and hash-config checksums. UAPI still accepts legacy v2 state manifests for compatibility, but v3 runtime execution requires any provided tokenizer projection to match the manifest checksum before canonical ngram lookups are built.
- Paper Engram row-prefetch plans now carry tokenizer-projection and hash-config checksums, and UAPI rejects a supplied row-prefetch plan when its contract does not match the active paper `ENGRAM_STATE` manifest.
- `sim-cli lingqu-memory build-engram-hash-config` now binds the hash config to the tokenizer projection artifact checksum (`aggregate_checksum`) instead of the source tokenizer-file checksum.
- `sim-cli lingqu-memory import-paper-engram-module` can import a complete paper Engram manifest bundle in dependency order (`projection`, `hash_config`, table shards, gates, optional recipe/eval, module), persist all registries, and resolve runtime artifacts as the import validation gate.
- `sim-cli lingqu-memory seed-paper-engram-fixture` now exposes explicit `--table-init` and `--gate-init` modes (`zero`, `fixture`, `random-normal`) so Phase 4 correctness/performance runs can separate no-op baseline, deterministic fixture mutation, and deterministic random-normal payloads.
- W5 `qwen3-engram-context` runtime logs now include row-prefetch hit/request counters, hit-rate milli, table/gate/indices bytes moved, hidden injection byte counters, backend latency, and output checksums.

The terminal-only context-op path is not enough. Paper Engram must be able to
inject at configured model layers, not only after terminal hidden.

## Post-Training And Fine-Tuning Support

The simulator should define artifact contracts even if training runs outside
this repo.

### Export Contract

An Engram training job should export:

```text
engram_module.json
tokenizer_projection.json
hash_config.json
table shards
gate/fusion weights
training_recipe.json
eval_report.json
```

`engram_module.json` is the entrypoint consumed by Lingqu Memory Service.

### Training Modes

1. **Engram-only continued pretrain**
   - freeze base model
   - train Engram tables and gate
   - good first proof because it isolates Engram contribution
2. **Engram + LoRA**
   - train Engram plus low-rank adapters
   - useful when business data requires model-space adaptation
3. **Full fine-tune**
   - highest cost and risk
   - not the first target for this simulator

### Acceptance For A Trained Table

Minimum required evidence before treating a table as useful:

1. The table loads through Memory Service by manifest.
2. CPU reference and backend output match for deterministic test inputs.
3. The table changes hidden/output checksums compared with zero table.
4. A held-out business validation set shows no regression versus base or
   decode-policy-only baseline.
5. Runtime counters prove deterministic prefetch has row-level locality and
   bounded latency.

## Phased Implementation Plan

### Phase 0: Documentation And Naming Cleanup

1. Mark current W4/W5 no-repeat Engram docs as decode-policy scaffolding.
2. Make this document the canonical paper-aligned Engram plan.
3. Update reports to distinguish:
   - `decode_policy_*`
   - `engram_context_*`
   - `paper_engram_*`

### Phase 1: Canonical Token Projection

1. Add tokenizer vocabulary ingestion.
2. Build and persist `P_model`.
3. Add tests for:
   - NFKC normalization
   - lowercasing
   - space-marker normalization
   - special-token isolation
   - stable checksum

### Phase 2: Hash/Index Contract

1. Define shared Rust/C hash implementation.
2. Add CPU tests against vendor-compatible fixtures.
3. Generate `{2,3}` ngram indices from canonical IDs.
4. Add exact-key rolling index for no-repeat decode policy as a separate
   consumer of canonical IDs.

### Phase 3: Memory Service Engram Artifacts

1. Add manifest structs and durable storage paths.
2. Add CLI commands:
   - import Engram module
   - validate Engram module
   - list Engram modules
   - materialize row-block ObjectRefs
3. Add restart/rebuild tests from durable manifests.

Current status: manifest registration/list/validation commands exist.
`import-paper-engram-module` imports complete manifest bundles in dependency
order and validates them by resolving runtime artifacts. `resolve-paper-engram-
table-row-blocks` resolves durable row-block refs without copying table payload
bytes. `plan-paper-engram-row-prefetch` can map known canonical token history
to table rows and backing block refs through the shared hash contract.
`publish-paper-engram-row-prefetch` exports that plan into the W5 Object Service
snapshot as metadata without publishing full table payloads. The row prefetch
ObjectRef is a formal W5 CLI/script input and is validated as an adjunct to the
paper `ENGRAM_STATE` entrypoint rather than as standalone runtime state.

### Phase 4: Paper-Compatible CPU Reference

1. Extend `EngramContextOp` from single `indices[B,8]` to multi-order,
   multi-head descriptors.
2. Support zero/random/fixture table modes for correctness and performance.
3. Keep trained-table mode manifest-backed only.

Current status: the CPU reference already accepts paper-style multi-order,
multi-head table descriptors and lookup refs. Fixture seeding can generate
explicit zero, deterministic fixture, or deterministic random-normal table and
gate payloads; trained table quality remains manifest-backed through module
quality claims.

### Phase 5: W5 Runtime Injection

1. Add layer-boundary insertion points.
2. Resolve canonical indices per decode step.
3. Route operands through Memory Service/Object Service.
4. Compare CPU reference and simpler-host backend.
5. Validate no model-quality claim for fixture tables.

### Phase 6: Trained Table Integration

1. Import post-training/fine-tuning artifacts.
2. Run W5 cluster inference with trained Engram table.
3. Compare:
   - base model
   - base + decode policy
   - base + paper Engram
   - base + paper Engram + decode policy

### Phase 7: Performance Path

1. Reuse vendor fused SIMT kernel where shape-compatible.
2. Add host/offload prefetch based on deterministic indices.
3. Use SIM_DEC/OBMM improvements for large table payload movement where needed.
4. Report row prefetch hit rate, table bytes moved, backend latency, and
   hidden injection overhead.

Current status: paper Engram context reports expose backend latency,
row-prefetch hit/request/rate counters, table/gate/indices bytes moved, and
hidden input/output/injection-overhead bytes in the W5 runtime log. Vendor fused
SIMT launch and large table movement optimization remain open.

## Open Decisions

1. Which tokenizer source is canonical for Qwen3 runs: local HF/modelscope
   tokenizer files, embedded vocabulary snapshot, or exported training
   artifact?
2. Should table layout be per-order/per-head separate tables or one global
   table with deterministic offsets?
3. Which layer placements are first-class for Qwen3-0.6B and Qwen3-14B?
4. What is the first acceptable training target: Engram-only continued
   pretrain, Engram-only SFT, or Engram+LoRA?
5. How large should the first business-domain table be before overfitting risk
   dominates?

## Non-Goals

1. Do not claim paper Engram quality gains with random or fixture tables.
2. Do not call no-repeat/repetition policy "paper Engram".
3. Do not make RAG document embeddings look like Engram tables; they have
   different construction, lookup, and training semantics.
4. Do not require fused SIMT before CPU reference and Memory Service semantics
   are correct.

## Near-Term Acceptance

The next milestone is documentation and contract alignment, not performance:

1. Existing docs clearly distinguish decode policy from paper Engram.
2. Memory Service has a documented Engram artifact model.
3. W5 has a documented rebase path from terminal context-op to layer-level
   hidden-state injection.
4. Training/fine-tuning outputs have a documented import contract.
5. Fixture-table runs are labeled system/performance validation only.
