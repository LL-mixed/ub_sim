# Paper Engram 论文解读与当前设计说明

本文解释 `Engram_paper.pdf` 中 Engram 的核心思想，并逐项说明
`docs/plans/2026-05-22-paper-engram-alignment-plan.md` 中的设计。

结论先行：

```text
paper Engram 不是 no-repeat 策略。
paper Engram 是插入 Transformer 中间层的 conditional memory module。
当前 repo 里已有的 no-repeat/repetition 逻辑只能算 decode policy。
当前 repo 里的 Engram context op 只覆盖 paper Engram 的后半段算子。
真正对齐 paper，需要补 tokenizer compression、canonical ngram、hash index、
训练得到的 Engram table、Memory Service artifact、W5 layer-level injection。
```

## 1. 论文 Engram 到底是什么

论文的核心问题是：Transformer 处理语言时，很多局部、静态、模式化的知识并不一定需要每次都靠深层计算重新“推出来”。例如固定短语、实体、局部搭配、公式化表达，这些更像可以通过 key 查表得到的静态记忆。

MoE 是 conditional computation：

```text
根据当前 hidden state 选择少数 expert 参与计算
```

Engram 是 conditional memory：

```text
根据当前 token suffix ngram 选择少数 memory rows 参与 hidden-state 增强
```

它的作用位置不是最终 sampler，而是 Transformer 中间层：

```text
H(l)
  -> Engram lookup
  -> gate/fusion
  -> H(l) + EngramOutput
  -> 后续 Attention / MoE / FFN
```

这意味着 Engram 会改变后续层看到的 hidden state，最终 logits 会自然变化。它不是在 logits 出来之后禁止某个 token。

## 2. 论文里的完整流程

### 2.1 Tokenizer Compression

普通 tokenizer 为了无损复原文本，会把语义相近但表面形式不同的 token 分成不同 ID。例如：

```text
"Apple"
" apple"
"APPLE"
```

这些 raw token ID 不一定相同。论文引入投影函数：

```text
P: V -> V'
```

把 raw token ID 映射到 canonical token ID。投影方式包括：

- NFKC normalization
- lowercasing
- 空格标记等 tokenizer-specific normalization

这样 Engram 的 key 不直接基于 raw token ID，而是基于 canonical ID：

```text
x_t' = P(x_t)
```

价值是提升语义密度，让 `Apple` 和 ` apple` 这类等价形式更容易共享同一类 ngram memory。

### 2.2 Canonical Suffix Ngram

在位置 `t`，取当前位置结尾的 canonical suffix ngram：

```text
g(t, n) = (x'_{t-n+1}, ..., x'_t)
```

论文主要使用 `{2,3}`-gram：

```text
2-gram: (x'_{t-1}, x'_t)
3-gram: (x'_{t-2}, x'_{t-1}, x'_t)
```

这一步把局部上下文转成 memory lookup key。

### 2.3 Multi-Head Hashing

所有可能的 2-gram/3-gram 组合空间极大，不可能为每个组合直接分配独立参数。因此论文用多头 hash：

```text
row = hash(order=n, head=k, canonical_ngram) % table_rows
```

每个 ngram order 有多个 hash heads。每个 head 查一行 embedding table。hash collision 是允许的，因为它是训练时共同学习出来的参数共享方式，而不是 correctness bug。

注意：这点只适用于 Engram table lookup。no-repeat 这种 correctness policy 不能接受 hash collision 导致误判，所以必须保留 exact key/tag。

### 2.4 Engram Table Lookup 和 Fusion

查到多行 table 后，论文会构造 memory vector，然后通过当前 hidden state 做 gate，再残差加回 hidden：

```text
memory = aggregate(table[indices])
gate = sigmoid(f(hidden))
output = hidden + gate * memory
```

当前 `crates/sim-models/src/engram_context.rs` 和 vendor `engram_simt` 更接近这一段。但它们假设 indices 已经存在，没有实现前面的 tokenizer compression 和 canonical ngram hashing。

### 2.5 Engram Table 是训练参数

Engram table 不是 RAG 的文档 embedding table。它更像一个超大的 embedding layer：

```text
key = canonical ngram hash
value = learned parameter row
```

训练时，LM loss 反向传播到被激活的 Engram rows 和 gate/fusion 参数。没有训练得到的 table，paper Engram 只能跑机制和性能，不能期待质量收益。

## 3. 当前 repo 与论文的差距

当前 repo 有三种被叫作 Engram 的东西。

### 3.1 Decode Policy

位置：

- `crates/sim-cli/src/main.rs`
- `guest-linux/aarch64/apps/w4_guest/w4_guest.c`

功能：

```text
candidate tokens + history
  -> no-repeat ngram / repetition penalty
  -> selected token
```

它发生在 logits/candidates 之后，是 sampler 层策略，不是 paper Engram。

### 3.2 Engram Context Op

位置：

- `crates/sim-models/src/engram_context.rs`
- `docs/plans/2026-05-16-w5-engram-simpler-host-context-op-design.md`
- `vendor/pto-isa/kernels/manual/a5/engram_simt/`

功能：

```text
indices + table + hidden + gate_weight
  -> gather
  -> mean
  -> gate
  -> residual output
```

它覆盖 paper Engram 的后半段算子，但没有 indices 生成、table 训练、Memory Service artifact。

### 3.3 Paper Engram

目标形态：

```text
raw token ids
  -> tokenizer compression
  -> canonical ngram
  -> multi-head hash
  -> trained table lookup
  -> hidden-state injection
```

这是 alignment plan 要把 repo 迁移到的方向。

## 4. Alignment Plan 逐节解释

### 4.1 Goal

目标是把 simulator 的 Engram 从“decode-time policy”升级为 paper-compatible conditional memory module。

这要求：

- 能在训练/后训练时生成 Engram table。
- Memory Service 能存储和服务 Engram table。
- W5 cluster inference 能消费这些 table。
- 当前 no-repeat policy 只保留为 sampler constraint，不再代表 Engram 本体。

### 4.2 Current Terminology Reset

这一节是为了修正命名混乱。

以后文档和实现应该区分：

```text
paper_engram_*      真正论文 Engram
engram_context_*    gather/gate/residual 算子
decode_policy_*     no-repeat/repetition/blocked-token 策略
```

这样做的用户影响是：跑 W5 validation 时，一眼能看出当前是在验证模型层 memory module，还是只是在验证 sampler policy。

### 4.3 Design Objectives

四个目标分别对应四条工程线。

第一，训练时构造 Engram table：

```text
base model + Engram module + business data -> trained Engram table
```

第二，Memory Service 支持 Engram table：

```text
table payload
projection
hash config
gate weights
module manifest
```

第三，支持后训练和微调：

```text
continued pretrain / SFT -> export Engram artifact bundle
```

第四，W5 rebase：

```text
当前 no-repeat Engram -> decode policy
当前 context-op -> paper Engram backend
新增 layer-level injection
```

### 4.4 Tokenizer Compression

设计里要求新增：

```text
P_model: raw_token_id -> canonical_token_id
```

这是 paper Engram 的第一步。它必须是 model-bound artifact，因为不同模型 tokenizer 不同，canonical projection 也不同。

为什么要持久化：

- 训练和推理必须完全一致。
- hash index 依赖 canonical ID。
- projection checksum 要写入 hash config 和 Engram module manifest。

特殊 token 要隔离处理。原因是 special token 的语义来自协议，不是普通文本。如果把 special token 和普通文本 token 合并，训练和推理都会被污染。

### 4.5 Canonical Ngram Indexing

这一节定义：

```text
orders = {2, 3}
```

decode 时不能每步扫描全历史，而应该维护 rolling canonical token history。每生成一个 token，只更新当前位置相关的 2-gram/3-gram key。

训练时可以由 dataloader 预计算，也可以运行时生成。关键是训练和推理必须共用同一套 projection/hash 规则。

### 4.6 Multi-Head Hashing

这节定义 repo-wide hash contract。

vendor 里已有 `compute_ngram_key_host()` 和 `multi_head_hash_host()`，但它还不是主线公共契约。alignment plan 要求把 hash 配置变成 artifact：

```text
orders
heads per order
table rows
hash seeds/primes
version
projection checksum
```

原因是 Engram table 是训练出来的。只要 hash 规则变了，table row 语义就变了。

### 4.7 Trained Engram Tables

这节明确 Engram table 是模型参数，不是 RAG index。

推荐第一种训练模式：

```text
freeze base model
train Engram tables + gate/fusion
```

原因：

- 工程风险最低。
- 可以单独评估 Engram 是否有效。
- 不会一开始就让 base model、LoRA、Engram table 的贡献混在一起。

fixture/random/zero table 只能用于系统验证：

```text
correctness
latency
prefetch
transport
backend parity
```

不能用于宣称模型质量提升。

### 4.8 Hidden-State Injection

这是当前 W5 最大的结构差距。

paper Engram 要插入 Transformer layer boundary：

```text
layer 2 hidden
  -> Engram
  -> layer 2/3 后续计算
```

而当前 no-repeat policy 是：

```text
terminal logits/candidates
  -> policy
  -> selected token
```

所以 terminal-only context-op 不够。W5 需要在 range/layer boundary 上能调用 Engram context op，然后把增强后的 hidden 继续传给后续 layer。

### 4.9 Memory Service Requirements

Memory Service 需要把 Engram 当成模型 artifact family，而不是普通临时对象。

设计拆成五类：

```text
tokenizer projection
hash config
table payloads
gate/fusion weights
module manifest
```

这样拆的原因：

- projection/hash 是索引语义。
- table/gate 是模型参数。
- module manifest 绑定 base model checkpoint、tokenizer、table layout、训练来源和 checksum。

Memory Service 的职责不是训练 table，而是：

```text
存储
版本化
校验
按语义解析
为运行时生成 ObjectRefs
为 deterministic lookup 做 prefetch plan
```

### 4.10 Engram Module Manifest

manifest 是训练产物和推理运行之间的合同。

关键字段包括：

```text
base_model_id
base_checkpoint_checksum
tokenizer_projection_ref
hash_config_ref
layers[]
orders[]
heads_per_order
hidden_size
table_dtype
gate_kind
quality_claim
payload_checksums
```

`quality_claim` 很重要。它防止 fixture table 被误用为有质量收益的 Engram：

```text
none       系统验证
posttrain 继续预训练
finetune  业务 SFT
imported  外部产物
```

### 4.11 Runtime Serving

推理时 Memory Service 要能做几件事：

1. 按 `(model_id, engram_id)` 找到 Engram module。
2. 下发 projection/hash config。
3. 根据 row range 找 table blocks。
4. 根据 deterministic ngram indices 做 prefetch plan。
5. 给 W5 runtime 生成 ObjectRefs。

这和 SIM_DEC/OBMM 的关系是：table bytes 可能在 host DRAM、Object Service、durable block、imported memory 中，但 Memory Service 管语义身份和放置策略。

### 4.12 W5 Rebase Plan

保留的东西：

- Object Service transport
- `EngramStateObjectRef`
- `engram_context.rs` CPU reference
- `simpler-host` backend
- vendor fused SIMT 探索

重分类的东西：

```text
no_repeat_ngram
repetition_penalty
blocked_token_ids
```

这些归入 decode policy。

新增的东西：

```text
range/layer boundary hidden
  -> canonical ngram indices
  -> Memory Service resolves/prefetches rows
  -> context op computes augmented hidden
  -> downstream Qwen layers continue
```

这才是 W5 对齐 paper Engram 的核心路径。

### 4.13 Post-Training And Fine-Tuning Support

训练可以在 repo 外做，但 repo 必须定义 import/export contract。

训练 job 至少导出：

```text
engram_module.json
tokenizer_projection.json
hash_config.json
table shards
gate/fusion weights
training_recipe.json
eval_report.json
```

这样 Memory Service 才能导入、校验、服务这些产物。

### 4.14 Training Modes

三个模式按风险递增：

1. Engram-only continued pretrain
2. Engram + LoRA
3. Full fine-tune

第一阶段推荐 Engram-only，因为它能隔离 Engram 的贡献。如果连冻结 base model 的 Engram-only 都没有收益，直接上 full fine-tune 很容易把问题掩盖掉。

### 4.15 Acceptance For A Trained Table

不能只要 table 能加载就认为有用。至少要证明：

- manifest 能通过 Memory Service 加载。
- CPU reference 和 backend 输出一致。
- table 不是空壳，能改变 hidden/output checksum。
- held-out business validation 不劣于 baseline。
- runtime prefetch/latency 可控。

这把“系统可跑”和“模型有效”分开了。

## 5. 分阶段计划解释

### Phase 0: Documentation And Naming Cleanup

目标是先把概念纠正。否则后面实现时会继续把 no-repeat policy 当成 Engram。

应产出：

```text
decode_policy_*
engram_context_*
paper_engram_*
```

### Phase 1: Canonical Token Projection

实现 tokenizer compression。

重点测试：

- NFKC
- lowercasing
- space marker normalization
- special token isolation
- checksum stability

这是后续所有 hash/table 的根。

### Phase 2: Hash/Index Contract

实现共享 hash 和 `{2,3}` index 生成。

这里同时可以把 no-repeat policy 优化为 O(1) rolling exact-key index，但它必须作为 decode policy 的消费者，而不是 paper Engram table lookup。

### Phase 3: Memory Service Engram Artifacts

让 Memory Service 能导入、验证、列出、物化 Engram artifacts。

这一步还不需要真正改 W5 forward，但要把 artifact contract 固化下来。

### Phase 4: Paper-Compatible CPU Reference

扩展 `EngramContextOp`：

```text
from: indices[B,8]
to:   multi-order + multi-head descriptor
```

CPU reference 是所有 backend 的正确性锚点。

### Phase 5: W5 Runtime Injection

把 Engram 插到 W5 layer boundary。

重点不是性能，而是语义正确：

- 同一份 table/indices，CPU reference 和 simpler-host 一致。
- fixture table 只能标注为 system validation。
- downstream layer 确实消费 augmented hidden。

### Phase 6: Trained Table Integration

导入真实后训练或 fine-tune 产物。

对比矩阵：

```text
base model
base + decode policy
base + paper Engram
base + paper Engram + decode policy
```

这样才能分清收益来自哪里。

### Phase 7: Performance Path

最后再做性能：

- 复用 vendor fused SIMT。
- 根据 deterministic indices 做 host/offload prefetch。
- 结合 SIM_DEC/OBMM 路径搬运大表。
- 汇报 row prefetch hit rate、table bytes、backend latency、hidden injection overhead。

性能放后面，是因为没有正确 artifact 和 reference，快也没有意义。

## 6. 与 RAG 的关系

Engram table 不是 RAG embedding table。

| 项 | RAG | Paper Engram |
| --- | --- | --- |
| 表项来源 | 文档 chunk encoder 输出 | 训练得到的模型参数 |
| 查询方式 | query embedding 相似度检索 | canonical ngram deterministic hash |
| 是否反向传播更新 | 通常不更新检索库 | 被激活 rows 参与训练 |
| 作用位置 | prompt/context/cross-attn | Transformer hidden state |
| 系统优化 | 依赖 ANN/search/cache | deterministic row prefetch |

这就是为什么 plan 要求训练产物 manifest，而不是只导入一批文档 embedding。

## 7. 对当前工作的直接影响

1. 当前默认 no-repeat trigram 可以继续保留，但要改名为 decode policy。
2. 当前 `EngramStateObjectRef` 可保留，但语义要从“decode state”扩展到“paper Engram module/view ref”。
3. 当前 simpler-host context-op 可以继续用，但要改造成 paper-compatible backend。
4. 当前 Memory Service 需要新增 Engram artifact family。
5. 当前 W5 terminal context-op 不够，需要 layer-level injection。
6. 没有训练 table 时，只能做系统和性能验证，不能声明模型质量提升。

## 8. 最小可执行路线

建议下一步按这个顺序落地：

```text
1. 完成命名清理：paper_engram / engram_context / decode_policy
2. 实现 tokenizer projection artifact
3. 实现 canonical ngram + shared hash
4. Memory Service 支持 Engram module manifest
5. CPU reference 支持 multi-order/multi-head
6. W5 layer boundary 接入 fixture table
7. 导入真实训练产物
8. 再接 fused SIMT / prefetch / OBMM 性能路径
```

这条路径能避免两个错误：

- 把 no-repeat policy 误认为 paper Engram。
- 在没有训练 table 和 reference correctness 的情况下先做性能优化。
