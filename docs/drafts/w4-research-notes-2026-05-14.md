# W4 8-node guest decode 优化

日期: 2026-05-15

# multi-node stream decoding 优化方向

待展开



## 当前进展

W4 8-node guest decode 的首要优化对象不是单个 layer 的数值计算，而是跨 node handover 和 pipeline 等待。

当前 8-node pipeline 中，step0 是 TTFT，包含 cold init、prefill full hidden handoff、KV cache 初次发布和 round barrier。step1 及之后是 TPOT，已经进入热路径，使用持久化真实数值 KV cache，并只传 decode token hidden slice。

2026-05-15 更新: `88ac70d Use object refs for W4 Qwen3 handoff` 已把 guest handover descriptor 切到 ObjectRef wire contract。guest 侧不再把 hidden/KV 大 payload 写入 UAPI segment；UAPI descriptor 携带 ObjectRef，sim-uapi adapter resolve/materialize object-backed operand。当前仍保留一次 adapter 内部 materialize 为旧 `run_w4_chipbackend(&[u8])` slice 的兼容层，backend trait 完全改成 object-backed operand view 是下一步接口收口。

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

0.6B / 8-node / 16 steps:

- PASS。
- Summary: `guest-linux/aarch64/out/eight_node_w4_guest_summary.2026-05-15_17-17-46_w4guest8_13949.txt`。
- TTFT: `8556ms`。
- TPOT step1-step14 平均: `3360ms/token`。
- TPOT step1-step14 中位数: `3318ms/token`。
- TPOT step1-step14 范围: `3033ms-3887ms/token`。
- 输出 token ids: `[11, 358, 2776, 264, 2699, 21815, 911, 279, 7286, 315, 330, 265, 2719, 1, 304, 279]`。
- 输出 text: `, I'm a bit confused about the concept of "reality" in the`。

14B / 8-node / 16 steps:

- PASS。
- Summary: `guest-linux/aarch64/out/eight_node_w4_guest_summary.2026-05-15_17-20-14_w4guest8_8624.txt`。
- TTFT: `85364ms`。
- TPOT step1-step14 平均: `22874ms/token`。
- TPOT step1-step14 中位数: `22881ms/token`。
- TPOT step1-step14 范围: `22147ms-23980ms/token`。
- 输出 token ids: `[11, 358, 2776, 4460, 311, 3535, 279, 7286, 315, 330, 1782, 1879, 374, 264, 1467, 1]`。
- 输出 text: `, I'm trying to understand the concept of "the world is a text"`。

### 1.4 判断

`#1 Handover 数据模型` 从功能正确性角度已经完成：

- 不再走 synthetic hidden handoff。
- step0/full hidden 与 step1+/decode hidden 的 byte contract 已 profile 化。
- descriptor、DB service、guest verifier、UAPI contract 都按同一模型校验。
- 0.6B 和 14B 都通过 8-node 16-step guest decode。

残余工作不再属于“数据模型是否正确”，而是性能优化：

- step0 full hidden prefill handoff 仍然很重。
- step0 cold init 和 round barrier 占 TTFT 主体。
- step1+ 的 TPOT 仍主要受 pipeline input wait 影响。

## 2. Timing 观察

### 2.1 0.6B timing

| step | round_ms | 说明 |
| ---: | ---: | --- |
| 0 | 8556 | TTFT，cold init + full hidden prefill handoff |
| 1 | 3033 | TPOT |
| 2 | 3076 | TPOT |
| 3 | 3066 | TPOT |
| 4 | 3066 | TPOT |
| 5 | 3130 | TPOT |
| 6 | 3348 | TPOT |
| 7 | 3322 | TPOT |
| 8 | 3270 | TPOT |
| 9 | 3313 | TPOT |
| 10 | 3372 | TPOT |
| 11 | 3538 | TPOT |
| 12 | 3735 | TPOT |
| 13 | 3881 | TPOT |
| 14 | 3887 | TPOT |
| 15 | 2826 | final token，缺少后续 barrier，不适合算稳态 |

step0 bottleneck:

- `max_input_wait_ms=4104`
- `max_compute_window_ms=1142`
- `max_barrier_ms=4988`

step1-step14 bottleneck:

- `max_input_wait_ms` 平均 `2356ms`
- `max_compute_window_ms` 平均 `832ms`
- `max_barrier_ms` 平均 `3116ms`
- OBMM high-water: `7373792` bytes / node max，`max_payload_used_pct_milli=1373`

### 2.2 14B timing

| step | round_ms | 说明 |
| ---: | ---: | --- |
| 0 | 85364 | TTFT，cold init + full hidden prefill handoff |
| 1 | 23980 | TPOT |
| 2 | 22147 | TPOT |
| 3 | 22491 | TPOT |
| 4 | 22469 | TPOT |
| 5 | 22580 | TPOT |
| 6 | 22675 | TPOT |
| 7 | 22747 | TPOT |
| 8 | 22880 | TPOT |
| 9 | 22882 | TPOT |
| 10 | 22897 | TPOT |
| 11 | 23126 | TPOT |
| 12 | 23116 | TPOT |
| 13 | 22995 | TPOT |
| 14 | 23244 | TPOT |
| 15 | 22083 | final token，缺少后续 barrier，不适合算稳态 |

step0 bottleneck:

- `max_input_wait_ms=68935`
- `max_compute_window_ms=12987`
- `max_barrier_ms=72785`

step1-step14 bottleneck:

- `max_input_wait_ms` 平均 `17534ms`
- `max_compute_window_ms` 平均 `5134ms`
- `max_barrier_ms` 平均 `20465ms`
- OBMM high-water: `10053576` bytes / node max，`max_payload_used_pct_milli=1873`

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

第一版 ObjectRef handover 已完成:

- Handover contract 传 ObjectRef，不再传大 tensor 内容。
- guest 侧 Lingqu object service 建在 OBMM shmem 上，负责 object metadata、owner、checksum、版本信息。
- UAPI descriptor 声明 input/output ObjectRef，sim-uapi adapter resolve/materialize。
- W4 guest decode 不再靠日志和 payload scan 证明“像真的”，数据路径已经进入 object-ref 运行形态。

仍未完成的接口收口:

- `run_w4_chipbackend(&[u8])` 仍是 flat slice contract，adapter 内部还要 assemble 一次 host-side input view。
- backend 还不能直接接收 object-backed operand descriptor。
- checksum/metadata commit 仍由 adapter/object service 侧承接，backend 还没有直接声明 output object 写入结果。

## 4. 待跟进事项

1. [x] 修正 Handover 数据模型: step0 full hidden，step1+ token-slice hidden。
2. [x] 持久化真实数值 KV cache，并在 decode step resolve previous-step KV state。
3. [x] 用 0.6B 8-node 8-step 验证 TTFT/TPOT。
4. [x] 用 14B 8-node 8-step 验证 TTFT/TPOT。
5. [x] 用 0.6B 8-node 16-step 验证 TTFT/TPOT。
6. [x] 用 14B 8-node 16-step 验证 TTFT/TPOT。
7. [x] ObjectRef handover 第一版: UAPI descriptor carries ObjectRef，adapter resolve/materialize。
8. [ ] 拆分 TTFT: cold init、full hidden handoff、barrier 的独立 timing。
9. [ ] 优化 TPOT input wait: descriptor wait、metadata resolve、copy path。
10. [ ] 将 backend trait 收口为 object-backed operand view，去掉 adapter 内部 flat slice assemble。
11. [ ] 优化/收敛 14B TPOT 中约 `17.5s` input wait 和约 `20.5s` barrier。
12. [ ] 评估单 node 多 layer fusion 的实际收益。

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
