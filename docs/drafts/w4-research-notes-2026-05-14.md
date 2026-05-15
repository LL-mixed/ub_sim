# W4 8-node guest decode 优化

日期: 2026-05-15

# multi-node stream decoding 优化方向

待展开



## 当前进展

W4 8-node guest decode 的首要优化对象不是单个 layer 的数值计算，而是跨 node handover 和 pipeline 等待。

当前 8-node pipeline 中，step0 是 TTFT，包含 cold init、prefill full hidden handoff、KV cache 初次发布和 round barrier。step1 及之后是 TPOT，已经进入热路径，使用持久化真实数值 KV cache，并只传 decode token hidden slice。

## 1. Handover 数据模型

状态: 已完成第一版真实数据模型，提交 `ff85735 Support decode hidden handoff sizing`。

### 1.1 Contract

Handover 传的是真实数值数据，不是 synthetic payload。

- step0 / prefill:
  - hidden handoff 使用 full hidden range。
  - 0.6B: `262144` bytes。
  - 14B: `1310720` bytes。
- step1+ / decode:
  - hidden handoff 使用 token-slice hidden bytes。
  - 0.6B: `2048` bytes。
  - 14B: `10240` bytes。
- KV cache:
  - 每个 node 持久化并发布本 node layer range 的真实数值 KV cache。
  - decode step 会 resolve previous-step KV state，并生成 current-step KV state。

### 1.2 已落地范围

- `qwen3_dense` profile 暴露 `decode_hidden_bytes`。
- CLI 和 guest scripts 统一下发 `SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES`。
- guest dispatch descriptor、payload verify、range forward table 支持 step0 full hidden 与 step1+ token-slice hidden。
- DB service 的 publish/wait/runtime descriptor matching 支持按 decode step 匹配 handoff hidden bytes。
- 14B contract 测试覆盖 `hidden_range_bytes=1310720` 与 `decode_hidden_bytes=10240`。

### 1.3 验证结果

0.6B / 8-node / 8 steps:

- PASS。
- TTFT: `65260ms`。
- TPOT step1-step6 平均: `3755ms/token`。
- TPOT step1-step6 中位数: `3808ms/token`。
- 输出 pieces: `,ĠI'mĠaĠbitĠconfusedĠaboutĠthe`。

14B / 8-node / 8 steps:

- PASS。
- TTFT: `384089ms`。
- TPOT step1-step6 平均: `25355ms/token`。
- TPOT step1-step6 中位数: `24896ms/token`。
- 输出 pieces: `,ĠI'mĠtryingĠtoĠunderstandĠtheĠconcept`。

### 1.4 判断

`#1 Handover 数据模型` 从功能正确性角度已经完成：

- 不再走 synthetic hidden handoff。
- step0/full hidden 与 step1+/decode hidden 的 byte contract 已 profile 化。
- descriptor、DB service、guest verifier、UAPI contract 都按同一模型校验。
- 0.6B 和 14B 都通过 8-node 8-step guest decode。

残余工作不再属于“数据模型是否正确”，而是性能优化：

- step0 full hidden prefill handoff 仍然很重。
- step0 cold init 和 round barrier 占 TTFT 主体。
- step1+ 的 TPOT 仍主要受 pipeline input wait 影响。

## 2. Timing 观察

### 2.1 0.6B timing

| step | round_ms | 说明 |
| ---: | ---: | --- |
| 0 | 65260 | TTFT，cold init + full hidden prefill handoff |
| 1 | 3571 | TPOT |
| 2 | 3859 | TPOT |
| 3 | 3606 | TPOT |
| 4 | 3836 | TPOT |
| 5 | 3780 | TPOT |
| 6 | 3878 | TPOT |
| 7 | 2812 | final token，缺少后续 barrier，不适合算稳态 |

step0 bottleneck:

- `max_input_wait_ms=60732`
- `max_compute_window_ms=1166`
- `max_barrier_ms=61697`

step1-step6 bottleneck:

- `max_input_wait_ms` 约 `2486ms-2884ms`
- `max_compute_window_ms` 约 `819ms-891ms`

### 2.2 14B timing

| step | round_ms | 说明 |
| ---: | ---: | --- |
| 0 | 384089 | TTFT，cold init + full hidden prefill handoff |
| 1 | 27455 | TPOT |
| 2 | 24720 | TPOT |
| 3 | 24835 | TPOT |
| 4 | 24876 | TPOT |
| 5 | 24915 | TPOT |
| 6 | 25330 | TPOT |
| 7 | 22690 | final token，缺少后续 barrier，不适合算稳态 |

step0 bottleneck:

- `max_input_wait_ms=365593`
- `max_compute_window_ms=15070`
- `max_barrier_ms=371564`

step1-step6 bottleneck:

- `max_input_wait_ms` 约 `19367ms-21838ms`
- `max_compute_window_ms` 约 `5053ms-5345ms`

## 3. 后续优化方向

### 3.1 TTFT: 拆冷启动和 prefill handoff

目标: 把 step0 的真实瓶颈拆清楚，避免只看到一个巨大的 TTFT。

需要继续量化:

- QEMU/initramfs/guest app 启动时间。
- DB service cluster init 和 OBMM pool layout 时间。
- full hidden handoff copy/metadata/descriptor 等待时间。
- round barrier 等尾部 node 的时间。

可做优化:

- 将 DB service cluster、OBMM pool、queue activation 移到 decode round 外预热。
- 减少 step0 full hidden 的复制次数。
- 为 full hidden prefill handoff 建立更直接的 shared-buffer handoff path。

### 3.2 TPOT: 降低 pipeline input wait

TPOT 目前不是纯 compute 时间。step1+ 每个 token 主要由前序 node 逐段推进导致的 input wait 和当前 node compute window 组成。

可做优化:

- 优先减少 per-hop handoff metadata/descriptor wait。
- 评估 node 间 descriptor push/poll 的等待策略，减少 busy wait 和轮询间隔。
- 检查末端 node 的 compute/submit 抖动，尤其 14B 的 `max_submit_ms` 接近 compute window。

### 3.3 单 node 多 layer 融合

这仍是二级优化，不是当前最大瓶颈。

潜在收益:

- 减少每个 node 内多个 layer 的 dispatch/doorbell overhead。
- 可能降低 compute window，但不会直接消除跨 node input wait。

需要先确认:

- 当前 simpler backend 是否能表达 fused layer runtime。
- fused kernel 是否能复用现有真实权重切片和 KV cache layout。

### 3.4 Backend 直接操作 OBMM pool / Object Service

长期方向是让 backend 直接消费 OBMM pool 中的对象，避免 guest/UAPI segment 与 object payload 之间的中间复制。

需要明确:

- backend 是否能获得 object descriptor 的稳定地址和生命周期。
- object payload 是否允许 backend 原地读写。
- checksum/metadata 更新由 backend 负责还是 DB service 负责。

## 4. 待跟进事项

1. [x] 修正 Handover 数据模型: step0 full hidden，step1+ token-slice hidden。
2. [x] 持久化真实数值 KV cache，并在 decode step resolve previous-step KV state。
3. [x] 用 0.6B 8-node 8-step 验证 TTFT/TPOT。
4. [x] 用 14B 8-node 8-step 验证 TTFT/TPOT。
5. [ ] 拆分 TTFT: cold init、full hidden handoff、barrier 的独立 timing。
6. [ ] 优化 TPOT input wait: descriptor wait、metadata resolve、copy path。
7. [ ] 评估单 node 多 layer fusion 的实际收益。
8. [ ] 评估 backend 直接读写 OBMM pool/object payload 的架构改动。

# Appendix



## 一：单 step 内 node 的多 layer 融合

**核心问题**: 在一个推理 step 内部，单个 node 上多个 layer 的计算是否可以进行融合优化？

**思考方向**:

- 当前 layer-by-layer 的执行模式是否存在冗余？
- 多 layer 融合能否减少 kernel launch overhead？
- 对 memory access pattern 的影响？
- 是否需要在 simpler backend 层面支持 fused kernel？

---

## 二：node 间 hand over 时的数据传递

### 2.1 核心问题

当 node 与 node 之间需要进行 hand over（交接/切换）时，**KV Cache 和 Hidden State 的传递到底传递的是什么？**

### 2.2 关键疑问

- 传递的是**实际数据**（tensor 内容）还是 **Object Service 的 reference**？
- 如果是 reference，那么：
  - reference 的生命周期如何管理？
  - 跨 node 的 reference 如何解析？
  - 数据一致性如何保证？
- 如果是实际数据：
  - 数据量有多大？（KV Cache 通常占显存大头）
  - 传输延迟是否可接受？
  - 是否需要压缩/量化？

### 2.3 相关概念

- **Object Service**: W4 中的对象存储服务，可能用于跨 node 共享数据
- **KV Cache**: Transformer 推理中的键值缓存，存储历史 token 的 key/value
- **Hidden State**: 模型中间层的隐藏状态表示

---

## 三：Simpler Backend 直接操作共享内存 / Object Service

### 3.1 核心问题

**能否在 simpler 的 backend 那边，直接对 share memory 进行操作，甚至是对 object service 进行操作？**

### 3.2 思考方向

- **Share Memory 直接操作**:
  - 绕过传统的数据拷贝路径
  - 零拷贝（zero-copy）数据传输
  - 对性能的提升预期
  - 同步/并发控制问题

- **Object Service 直接操作**:
  - 直接读写 object service 中的对象
  - 是否需要新的 API 接口？
  - 与现有 simpler backend 架构的兼容性
  - 权限/安全模型

### 3.3 潜在收益

- 减少数据搬运开销
- 降低延迟
- 提高吞吐量
- 简化数据流

---

## 待跟进事项

1. [ ] 调研单 step 内多 layer 融合的技术可行性
2. [ ] 确认 node 间 hand over 的数据传递机制（reference vs 实际数据）
3. [ ] 评估 simpler backend 直接操作 share memory / object service 的架构影响
4. [ ] 确认 Object Service 在 host 侧的的具体接口和语义

---
