# UB Sim LLM Infer 端到端验证报告

日期：2026-05-26

## 结论

W5 当前已经在 W4 guest/QEMU 多节点底座上形成 Qwen3-14B LLM inference 的端到端闭环。最新稳定验证包含两轮 8-node、16-step cluster infer：

| 轮次 | run id | 目的 | 结果 |
| --- | --- | --- | --- |
| seed run | `2026-05-26_11-17-12_w5_qwen3_14b_decode_19535` | 完整 8-node range forward，产生 range outboundary observations / Memory Service decision artifacts | 16/16 steps 完成，8/8 nodes pass，`worker_timing_records=128`，`memory_boundary_observation_summary.records=112` |
| reuse run | `2026-05-26_12-04-43_w5_qwen3_14b_decode_26742` | 加载 seed run 的 boundary registry，验证 Memory Service lookup hit 与 `jump-to-terminal` 执行 | 16/16 steps 完成，8/8 nodes pass，`lookup_hits=16`，`boundary_hits=16`，`terminal_selects=16` |

当前主线状态：

- W4 报告仍然保留为 guest/QEMU resource-backed UAPI、OBMM object service、Qwen3-0.6B decode-loop 的底座验证文档，不应该删除。
- 本报告聚焦 W5 LLM inference：Qwen3-14B、8-node layer-range pipeline、Memory Service artifact access、Shortcut Path Jump、sampler/Engram policy 串行决策，以及 data store 的分层职责。
- 最新 reuse run 证明 shortpath 不是“只写日志”或“后处理 replay”：运行时通过 Memory Service staged registry 命中 verified terminal logits artifact，走 sampler selected token，再发布 terminal token。
- 最新 reuse run 没有启用 Engram policy：`engram_timing_records=0`，`prefix_cache_ids=none`。因此它验证的是已实现 Shortcut Path Jump，不是 paper Engram alignment，也不是 Prefix Cache。
- 当前仍有一个架构收敛点没有完成：artifact commit 需要彻底收敛到 decode 运行过程中的 async commit into Memory Service，而不是依赖 summary/log 后处理生成 staged decision store。

## 系统架构

W5 不是替换 W4，而是在 W4 已验证的 guest/QEMU/resource-backed UAPI 底座上增加 LLM inference runtime、Memory Service artifact plane 和 policy plane。

![W5 LLM infer architecture](./ub_sim_llm_infer_e2e_architecture.svg)

W5 的职责边界：

| 层级 | 职责 | 当前验证状态 |
| --- | --- | --- |
| guest worker | 消费 step-local work item，执行本 node layer range，发布 hidden/KV/token artifacts | seed/reuse 两轮均通过 |
| QEMU / UAPI | resource-backed descriptors、CQ completion、guest/QEMU 进程与串口日志 | 继承 W4 底座，W5 profile 继续使用 |
| OBMM / Lingqu Object Service | hidden/KV/logits/token payload 的热路径对象发布、解析、SPSC 通知 | seed run 记录 `hidden_backend=obmm_shmem`，reuse run 消费 Memory Service artifact |
| Memory Service | 记录 boundary observation、构建 lookup request、验证 artifact、返回 shortpath decision | reuse run `lookup_hits=16` |
| sampler policy | 从 terminal logits 选择 selected token | reuse run `qwen3_w5_memory_terminal_logits_selected` |
| Engram policy | 在 sampler 之后继续做去重/历史/状态相关的 selected token 调整 | latest run 未启用；已有 harness marker 和 tests 覆盖入口 |
| report tooling | 汇总 output、timing、hit/miss、store 状态 | 最新 summary 已覆盖 timing 和 Memory Service stages |

## Qwen3-14B W5 模型与 layer range

当前 `qwen3_14b_decode` profile 从 Qwen3-14B config 推导模型形态：

| 字段 | 数值 |
| --- | ---: |
| `vocab_size` | 151936 |
| `hidden_size` | 5120 |
| `intermediate_size` | 17408 |
| `num_hidden_layers` | 40 |
| `num_attention_heads` | 40 |
| `num_key_value_heads` | 8 |
| `head_dim` | 128 |
| `max_position_embeddings` | 40960 |
| `rope_theta` | 1000000 |
| `hidden_range_bytes` | 1310720 |
| `decode_hidden_bytes` | 10240 |
| `kv_state_bytes` | 327680 |

8-node balanced placement：

| node | layer range | 下游 |
| --- | --- | --- |
| nodeA / node1 | `[0,5)` | nodeB |
| nodeB / node2 | `[5,10)` | nodeC |
| nodeC / node3 | `[10,15)` | nodeD |
| nodeD / node4 | `[15,20)` | nodeE |
| nodeE / node5 | `[20,25)` | nodeF |
| nodeF / node6 | `[25,30)` | nodeG |
| nodeG / node7 | `[30,35)` | nodeH |
| nodeH / node8 | `[35,40)` | terminal logits/token |

seed run 的 `112` 条 boundary observations 来自：

```text
16 decode steps x 7 range-exit boundaries(node1->2 ... node7->8) = 112
```

## Full Forward 运行流程

完整 range-forward 轮次用于产生可复用 artifacts。每个 worker 本身不需要维护全局 step 状态；step、node range、输入输出对象引用都属于 work item。

![W5 LLM infer execution flow](./ub_sim_llm_infer_e2e_execution_flow.svg)

Full forward 的数据面发布点：

| artifact | 生产者 | 消费者 | backing / metadata |
| --- | --- | --- | --- |
| range runtime input | 上游 node | 下游 node | `obmm_shmem` + `lingqu_object_service` + `obmm_spsc` |
| range runtime output | 当前 node | 下游 node / Memory Service observation | `obmm_shmem` + object metadata |
| KV state | 每个 node | 下一 step 同 node work item | `obmm_shmem` + object metadata |
| terminal logits | nodeH | sampler / Memory Service execution artifact | object-backed artifact |
| terminal token result | terminal owner | 下一 step prompt/history | object service token record |
| boundary observation | node1..node7 range exit | Memory Service lookup/index builder | Memory Service catalog/audit |

seed run 关键结果：

```text
decode_steps_expected=16
decode_steps_observed=16
worker_timing_records=128
passed_nodes=8/8
idle_timing_records=0
memory_boundary_observation_summary.records=112
source=w5_guest_range_exit
hidden_backend=obmm_shmem
```

## Memory Service 与 Data Store

Memory Service 的目标不是把二进制 payload 文本化，也不是把所有 artifacts 拼进一个日志文件。正确分层是：hot path payload 走 Lingqu Object Service / OBMM shmem / SPSC queue；durable payload 走 Lingqu block / dfs payload refs；metadata 和 audit 进入 Memory Service catalog、manifest、audit log；兼容性 report artifact 只能保留 lightweight JSON summary 或 decision index。

当前涉及的数据类型：

| 类型 | 内容 | 应放位置 | JSON 是否合适 |
| --- | --- | --- | --- |
| hidden tensor | range handoff hidden bytes | OBMM shmem hot object；durable 时用 block/dfs ref | 不合适 |
| KV state | per-node/per-step KV cache | OBMM shmem hot object；durable 时用 block/dfs ref | 不合适 |
| logits artifact | terminal logits / candidate table | Object payload + Memory Service execution artifact metadata | payload 不合适，metadata 可以 |
| BoundaryObservation | step、node、layer range、hidden fingerprint、object ref | Memory Service catalog/audit | 可以 |
| BoundaryLookupRequest | model、position、range boundary、hidden fingerprint、allowed action | Memory Service request/audit | 可以 |
| ShortpathSupportRecord | verified artifact 支持什么 jump action | Memory Service durable audit | 可以 |
| ShortpathDecision | 某次 lookup 的决策、support id、proof checksum | Memory Service durable audit | 可以 |
| EngramStateObject | table/indices/gate/history 等 object refs 与 checksum | Memory Service catalog + Object refs | metadata 可以，payload 不合适 |
| PrefixCache plan | prefix range、KV refs、verification metadata | Memory Service catalog | metadata 可以 |

最新 reuse run 使用的 store 口径：

- Memory Service: `lingqu_memory_service`
- lookup backend: `staged_registry`
- shortpath action: `jump-to-terminal`
- registry count: `112`
- decision store: `guest-linux/aarch64/out/w5_memory_runtime_boundary_lookup.2026-05-26_11-17-12_w5_qwen3_14b_decode_19535.json`

这里必须明确边界：12:04 reuse run 证明了“上一轮 artifacts 可以被后一轮 Memory Service lookup 消费并触发 shortpath”。它还没有完全证明“decode 过程中 async commit 到 durable Memory Service 后，下一轮不经 post-run promote 直接消费”。后者是下一步验收项。

## Shortcut Path Jump

Shortcut Path Jump 的目标是在某个 range outboundary 命中 verified artifact 后，直接跳到 terminal path，而不是继续生成下游 range-forward work items。

当前 `jump-to-terminal` 执行流程已经在上面的 execution flow 图中展开：node1 到达 boundary 后构造 `BoundaryLookupRequest`，Memory Service 命中 verified terminal logits artifact，runtime 加载 logits artifact，sampler 选 token，必要时继续走 Engram policy，最后发布 step-local terminal token result，并且不再生成 node2..node8 的本 step range-forward work items。

这个语义有几个关键点：

- shortpath hit 是 step-local 的。step N 命中不保证 step N+1 继续命中。
- worker 不需要在 idle wait 时知道全局 step；worker 消费的是带 step/range 的 work item。
- 如果命中 shortpath，后续 node 的本 step work item 不生成。因此 nodeB..nodeH 在 reuse run 中表现为 `idle_no_work_item`，不是继续计算旧 step，也不是等待 nodeA 的全局状态。
- shortpath 不能无条件 replay sampled token；必须加载 terminal logits artifact，走 sampler，得到 selected token。
- 如果启用 Engram policy，流程是 sampler selected token -> Engram policy -> published token，不是二选一。

reuse run 的 Memory Service summary：

```text
service=lingqu_memory_service
records=120
steps=16/16
lookup_hits=16
hit_registry_indexes=0,7,14,21,28,35,42,49,56,63,70,77,84,91,98,105
hit_registry_steps=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
hit_positions=3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18
actions=jump-to-terminal
artifact_kinds=logits
```

guest worker shortpath summary：

```text
boundary_hits=16
terminal_selects=16
expected_hits=16
actual_range_forwards=16
actual_runtime_inputs=15
actual_runtime_outputs=0
shortpath_no_dispatch=112
shortpath_terminal_commits=112
shortpath_publish_hidden_zero=16
full_pipeline_range_forwards=128
full_pipeline_runtime_inputs=127
full_pipeline_runtime_outputs=128
```

`shortpath_no_dispatch=112` 对应 `16 steps x 7 downstream nodes`。含义是 downstream range-forward work item 不生成，而不是把一个全局 skip 状态广播给所有 worker。

## Sampler 与 Engram Policy

当前 token 决策链路必须按串行关系理解：

```text
terminal logits
-> sampler(top-k/top-p/temperature/greedy config)
-> selected token
-> optional Engram policy(no-repeat / repetition / history state)
-> published token
-> next-step prompt/history input
```

这意味着：

- sampler 负责从 logits 分布中选 candidate token。
- Engram policy 不是 non-greedy sampler 的开关，也不是 sampler 的替代品。
- Engram policy 启用时，它消费 sampler 的 selected token / candidate context / history state，再决定是否保留、替换或 fallback。
- Engram policy 不启用时，sampler selected token 直接成为 published token。

W5 当前已实现的 Engram 相关组件：

| 组件 | 作用 | 当前状态 |
| --- | --- | --- |
| token policy | no-repeat ngram、repetition penalty、stop token priority 等 decode-time policy | 单测和旧 run 覆盖 |
| Engram history object | 保存 token history，用于后续 step policy | object service path 已有 marker |
| Engram state object | Memory Service 中的 `EngramStateObject`，携带 table/indices/gate refs 与 checksum | Memory Service API 和 CLI path 已有 |
| Engram context op | `cpu-reference-object-ref` / `simpler-host-object-ref` context augmentation | 计划与部分验证已有，latest 12:04 run 未启用 |
| Engram owner | owner node 发布 owner-owned Engram state，避免 shortpath producer 必须等于 Engram owner | 设计已进入 W5 path |

latest 12:04 run 的 Engram 状态：

```text
engram_timing: unavailable reason=no_qwen3_engram_timing_records
engram_context_records=0
prefix_cache_ids=none
```

因此本次 timing 不能拿来判断 Engram context op 的性能，只能说明 non-Engram shortpath path 功能稳定。

## Prefix Cache 与 Shortpath 的关系

Prefix Cache 和 Shortcut Path Jump 共享一部分 Memory Service 基础设施，但不是同一个机制：

| 机制 | 触发位置 | 目标 | 当前验证 |
| --- | --- | --- | --- |
| Shortcut Path Jump | decode step N 的 range outboundary | 命中 verified terminal logits 后跳到 terminal token path | latest reuse run 已验证 |
| Prefix Cache | 通常在 step0/prefill 或 prefix 复用阶段 | 复用 prefix 对应 KV/cache，不重新计算前缀 | latest run 未验证 |
| Prefetch plan | range start / lookahead | 提前 materialize 可能要用的对象 | latest run 未验证 |

已有 shortpath 证明了 Memory Service 可以基于 boundary evidence 找到 verified execution artifacts，并在 runtime 中改变执行路径。Prefix Cache 可以复用这套 artifact/catalog/object-ref 基础设施，但它的语义是 KV/prefix reuse，不是 terminal logits jump。

## Decode 输出

两轮输出 token ids 一致：

```text
[264, 2813, 448, 3746, 431, 32365, 16928, 323, 264, 1550, 21117, 315, 431, 32365, 9162, 13]
```

decode text：

```text
 a company with strong R&D capabilities and a high proportion of R&D investment.
```

这个 fragment 语法连贯，token pieces 解码正常。两轮一致的原因是 reuse run 命中了 seed run 同一批 verified artifacts，并在当前实现下得到相同 terminal logits/sampler result；这不是要求所有 shortpath run 都必须和 full forward byte-for-byte 输出一致。

reuse run 每个 step 的 selected token：

| step | token | piece | runner_up | margin_milli |
| --- | ---: | --- | ---: | ---: |
| 0 | 264 | ` a` | 279 | 1103 |
| 1 | 2813 | ` company` | 8453 | 0 |
| 2 | 448 | ` with` | 429 | 0 |
| 3 | 3746 | ` strong` | 264 | 0 |
| 4 | 431 | ` R` | 18770 | 2492 |
| 5 | 32365 | `&D` | 609 | 3933 |
| 6 | 16928 | ` capabilities` | 22302 | 3886 |
| 7 | 323 | ` and` | 13 | 84 |
| 8 | 264 | ` a` | 702 | 409 |
| 9 | 1550 | ` high` | 3644 | 0 |
| 10 | 21117 | ` proportion` | 2188 | 0 |
| 11 | 315 | ` of` | 304 | 11198 |
| 12 | 431 | ` R` | 3412 | 1712 |
| 13 | 32365 | `&D` | 609 | 14390 |
| 14 | 9162 | ` investment` | 16849 | 2443 |
| 15 | 13 | `.` | 11 | 1416 |

## Timing 分析

seed run 是完整 8-node range-forward pipeline：

| 指标 | 数值 |
| --- | ---: |
| decode steps | 16/16 |
| active workers | 8/8 per step |
| worker timing records | 128 |
| idle timing records | 0 |
| slowest step | step0, 68059 ms |
| step1 | 59954 ms |
| step2..15 | 24402 ms .. 28397 ms |
| node worker total | nodeA 434903 ms .. nodeH 459273 ms |
| boundary observations | 112 |

reuse run 是 shortpath hit 后的 jump-to-terminal path：

| 指标 | 数值 |
| --- | ---: |
| decode steps | 16/16 |
| active workers | 1/8 per step |
| worker timing records | 16 |
| idle timing records | 112 |
| slowest step | step0, 11868 ms |
| step1..15 | 3158 ms .. 3338 ms |
| nodeA worker total | 60523 ms |
| nodeB..nodeH | `idle_no_work_item` |
| boundary hits | 16 |
| terminal selects | 16 |

reuse run step timing：

| step | round_ms | compute_window_ms | publish_ms | workers |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 11868 | 8113 | 203 | 1/8 |
| 1 | 3215 | 2983 | 164 | 1/8 |
| 2 | 3158 | 2944 | 158 | 1/8 |
| 3 | 3192 | 2956 | 171 | 1/8 |
| 4 | 3171 | 2950 | 169 | 1/8 |
| 5 | 3194 | 2952 | 186 | 1/8 |
| 6 | 3201 | 2947 | 204 | 1/8 |
| 7 | 3236 | 2957 | 223 | 1/8 |
| 8 | 3235 | 2958 | 215 | 1/8 |
| 9 | 3240 | 2956 | 226 | 1/8 |
| 10 | 3261 | 2981 | 233 | 1/8 |
| 11 | 3289 | 2993 | 248 | 1/8 |
| 12 | 3269 | 2971 | 253 | 1/8 |
| 13 | 3325 | 3009 | 264 | 1/8 |
| 14 | 3331 | 2995 | 279 | 1/8 |
| 15 | 3338 | 3007 | 286 | 1/8 |

性能含义：

- seed run 的每个 step 要驱动 8 个 node 做 range forward，关键路径在后续节点和 handoff 上累计。
- reuse run 的每个 step 在 node1 boundary 命中后，直接走 terminal logits artifact + sampler + token publish，后续 node 不生成 work item。
- 因此实际 range forward 从 `128` 降到 `16`，单 step 时间从 seed run 的 24s..68s 降到 reuse run 的 3.1s..11.9s。

## Correctness Guard

Correctness guard 的目标是防止 shortpath 命中错误 artifact 或跳过必要 token 决策。当前 guard 覆盖的关键约束：

- artifact 必须绑定 model / step / position / layer boundary / hidden fingerprint。
- `jump-to-terminal` 必须有 verified terminal logits artifact。
- terminal token 不能直接 replay 旧 sampled token，必须从 logits artifact 走 sampler。
- Engram policy 启用时，sampler selected token 之后还要进入 Engram policy。
- 不匹配或缺失 artifact 时 fail close，继续正常 range-forward，而不是发布不可靠 token。

latest reuse run 的 correctness 证据：

```text
qwen3_w5_memory_terminal_logits_loaded: status=ready
qwen3_w5_memory_terminal_logits_selected: policy=sampler status=ok
qwen3_w5_memory_shortpath_commit: status=ok
decode_steps_observed=16
passed_nodes=8/8
```

这说明 guard 可以保护本轮命中 case 的正确性。尚需单独验收的是 runtime async commit 产生的 artifacts 在下一轮被直接消费时，guard 是否仍然覆盖所有 miss/mismatch cases。

## 与 W4 报告的关系

`docs/w4_guest_qemu_e2e_validation_report.md` 是 W4 底座报告。它证明：

- guest/QEMU 多节点 resource-backed UAPI 闭环可用；
- `chipbackend`、`shmem`、`dfs`、`db`、`block` completion source 分类完整；
- OBMM object service 可以承载 hidden/KV/token publish；
- Qwen3 decode-loop 形态已经进入 guest/QEMU/simulator/ChipBackend/simpler-capi/simpler/guest-result 闭环；
- 8-node layer-range pipeline、KV state publish/resolve、terminal token publish 都有 W4 级验证。

本报告在 W4 之上的 W5 主线：

- 模型从 Qwen3-0.6B 扩展到 Qwen3-14B；
- pipeline 从普通 range-forward 扩展到 Memory Service artifact-aware execution；
- data plane 从 OBMM object service 扩展到 Lingqu Memory Service catalog/audit/object refs；
- policy plane 从 terminal sampler 扩展到 sampler + optional Engram policy；
- execution path 从 full forward 扩展到 verified Shortcut Path Jump。

## 最新运行记录

reuse run：

- summary: `guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-05-26_12-04-43_w5_qwen3_14b_decode_26742.txt`
- run dir: `guest-linux/aarch64/logs/2026-05-26_12-04-43_w5_qwen3_14b_decode_26742_headless8`
- profile: `qwen3_14b_decode`
- Memory Service: `lingqu_memory_service`
- lookup backend: `staged_registry`
- action: `jump-to-terminal`
- registry count: `112`
- decision store: `guest-linux/aarch64/out/w5_memory_runtime_boundary_lookup.2026-05-26_11-17-12_w5_qwen3_14b_decode_19535.json`

seed run：

- summary: `guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-05-26_11-17-12_w5_qwen3_14b_decode_19535.txt`
- run dir: `guest-linux/aarch64/logs/2026-05-26_11-17-12_w5_qwen3_14b_decode_19535_headless8`
- profile: `qwen3_14b_decode`
- 作用：完整执行 16 steps x 8 nodes，记录 16 steps x 7 outboundary observations。

## 已运行验证

最近一轮代码与文档更新前，相关验证已经通过：

```text
cargo test -p sim-memory -p sim-uapi -p sim-cli
/opt/homebrew/bin/pytest guest-linux/aarch64/tests/test_w5_artifact_prune.py guest-linux/aarch64/tests/test_w5_inference_run_report.py
zsh -n guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh guest-linux/aarch64/scripts/run_w5_cluster_config.sh guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh
git diff --check
```

其中 Rust tests 覆盖 `sim-cli`、`sim-memory`、`sim-uapi`；pytest 覆盖 W5 artifact prune 与 inference run report；shell syntax check 覆盖 W4/W5 guest run scripts。

## 下一步

下一步需要确认围绕 artifact lifecycle 收敛：

1. 让 range forward 过程中产生的 boundary observations、terminal logits、KV refs 在 decode 运行中 async commit 到 Memory Service。
2. 去掉依赖 summary/log post-run promote 的 staged decision store 路径，把它降级为兼容和调试路径。
3. 将大 payload 全部放入 Lingqu object/shmem/block/dfs 数据面，JSON 只保存 metadata、manifest、audit index。
4. 跑全新两轮 W5 16-step cluster infer：第一轮 runtime async commit，第二轮直接从 Memory Service durable store lookup 并执行 shortpath。
5. 在第二轮同时报告 output correctness、hit/miss、timing、store size、duplicate object growth、Engram enabled/disabled 两种 policy path。
