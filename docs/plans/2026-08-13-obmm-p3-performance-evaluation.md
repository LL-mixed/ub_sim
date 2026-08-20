# OBMM remote-load P3 ABI v2 性能评估结果

> 命名说明：当前机制名为 `async load`。文中小写 `p2b` 仅用于精确引用改名前的
> 远端 workspace 和历史 evidence 路径。

> 日期：2026-08-13；更新：2026-08-20
>
> 状态：**2-node formal acceptance、4/8-node 定向 scale-out、2,240-case
> 7-seed coarse runtime policy 和 1,960-case fine-grained formal boundary 已完成；
> 4,942-case full matrix 于 2026-08-14 按用户要求安全暂停**
>
> 设计基线：[P3 对比评估详细设计](p3-comparative-evaluation-detailed-design.md)

## 1. 结论

当前可以给出五条有证据边界的结论：

1. ABI v2 的 2-node formal acceptance 为 **49/49 pass**，P0、P1、submit/await、async load、P4
   五个 phase gate 全部通过；旧 ABI v1 的 49-case 结果没有复用。
2. 在 `1 µs remote latency + 100 µs useful compute + 4 coroutines` 的 scalar
   基准点，async load 相对 submit/await demand 在 2/4/8-node 的 cluster throughput 分别高
   `29.77%`、`35.13%`、`28.21%`。
3. 2-node coarse runtime policy 的 **2,240/2,240 canonical run 全部通过**。policy
   schema v2 按固定 guest-vCPU 下的 workload makespan 重聚合：transparent policy 为
   `sync 48 / async load 32`，explicit policy 为 `sync 48 / submit/await 7 / async load 25`。
4. fine-grained formal boundary 的 **1,960/1,960 canonical run 全部通过**。70 个
   endpoint 的 transparent policy 为 `sync 32 / async load 38`，explicit policy 为
   `sync 32 / submit/await 7 / async load 31`；三个 async load measured-fastest endpoint 因发布阈值不足
   fail closed 到 sync。
5. 完整 P3 sensitivity 仍未完成。4,942-case full matrix 负责覆盖 jitter、tail、
   failure、range 和更完整的 crossing，当前保持暂停；coarse bucket 不向未测区域外推。

![submit/await 与 async load 在 2、4、8 节点上的 cluster throughput 和相对收益](2026-08-13-obmm-p3-performance-results.svg)

## 2. 评估对象与公平性

### 2.1 Scalar 基准点

| 项目 | 取值 |
|---|---:|
| payload | 8 B |
| pattern | sequential |
| operations | 30,000 / node |
| warmup | 1,000 / node |
| modeled remote latency | 1 µs |
| useful compute | 100 µs / op |
| coroutines | 4 |
| seeds | 1..7 |
| submit/await demand | `async-poll`，inflight=8，lookahead=0 |
| async load demand | `async-load`，inflight=1，lookahead=0 |

submit/await demand 与 async load demand 都在真实消费点发起访问。submit/await lookahead 是单列实验，不能
并入 submit/await demand 后再与 async load 比较。

### 2.2 Cluster metric

4/8-node 不能只取 nodeA，也不能把各节点独立算出的 rate 简单相加。本报告统一使用：

```text
cluster_operations = sum(node.operations)
cluster_makespan = max(node.makespan_ns)
cluster_throughput = cluster_operations / cluster_makespan
```

也就是由最慢节点决定整组完成时间。`obmm-remote-load-scale-report` 会逐 run 校验：

- `OBMM_RUN_EVIDENCE` 恰好一条且 `qemu_destroyed=1`；
- node evidence 数量等于 topology node count；
- 每节点 checksum、operations、seed、mode、case、drain 和 terminal counters 正确；
- async load 的 pending/complete 等于 operations，EL0 context/scheduler counters 为正；
- async load 的 QEMU-owned context 和 `async_load_*_cycles` counters 全为零；
- 同 seed 的 submit/await 与 async load checksum 相同；
- QEMU、kernel、initramfs hash 在所有 run 中一致。

任一条件不满足，报告状态为 `invalid`，该 run 不进入结论。

## 3. 产物身份

| Artifact | SHA-256 |
|---|---|
| QEMU | `362e7745d3fa6e55bdbdb6f33438ef2a224c64d82061a0da14d7ce3325b2958c` |
| guest kernel | `8f187f08ba0c28260ab5b6267f8dfeeee0e229938755b36e42596f684b25ccbb` |
| initramfs | `4cc0642a1b15daa607956c63ffd94af09dcd3409dd132270f27b7838771c4c32` |
| acceptance matrix | `fnv1a64:feafb00c11aecf16` |

2/4/8-node scenario SHA-256 不同，因为 topology 文件不同；QEMU、kernel 和 initramfs
保持相同。正式数据在 `n4-910c` 的 ARM64 Linux 环境执行，本地开发机没有启动 QEMU。

## 4. 2-node formal acceptance

正式目录：

```text
out/obmm-remote-load/p3-acceptance-abi-v2-20260813-r1/
```

`validation.json` 的状态为 `pass`：49 个 raw run 全部有效，7-seed requirement 满足，
P0、P1、submit/await、async load、P4 gate 均为 `pass`，`invalid_reasons=[]`。

### 4.1 Band S：单节点 canonical summary

下表沿用 evaluator 的 canonical nodeA 口径，用于与既有 `scalar.csv` 一致：

| Case | Median req/s | Median makespan | 相对 submit/await demand |
|---|---:|---:|---:|
| S0 sync | 5,460.91 | 5.494 s | +78.22% throughput |
| S1 submit/await demand | 3,064.13 | 9.791 s | baseline |
| S2 submit/await lookahead=4 | 3,098.35 | 9.683 s | +1.12% throughput |
| S3 async load demand | 3,979.55 | 7.539 s | +29.88% throughput |

async load 对 submit/await demand 的 median makespan 降低约 `23.0%`。但在这个 1-µs 低延迟点，
同步 scalar 仍比 async load 快约 `37.2%`；异步路径的固定提交/调度开销大于可隐藏的 1-µs
remote wait。因此该点证明的是 **async load 优于当前 submit/await demand 实现**，并不证明异步机制
已经越过同步路径的 break-even。

submit/await lookahead=4 相对 submit/await demand 仅提高约 `1.12%`，对应 median makespan 减少
`108.154 ms`。由于 acceptance 只测了一个 latency 点，不能从这里推导 schedule-ahead
的完整 break-even 区间。

### 4.2 Band R：4-KiB range/page

| Case | Median req/s | Median makespan | 相对 sync range |
|---|---:|---:|---:|
| R0 sync range | 3,654.05 | 71.741 s | baseline |
| R1 submit/await range | 3,842.31 | 68.226 s | +5.15% throughput |
| R2 userfaultfd | 2,031.74 | 129.024 s | −44.40% throughput |

每个 case 处理 262,144 个 4-KiB logical operation，即 1 GiB payload。该 Band 只比较
相同 4-KiB 粒度；async load v2 的 1/2/4/8-byte scalar load 不进入该表。

## 5. 2/4/8-node submit/await 与 async load scale-out

### 5.1 Cluster throughput

| Nodes | submit/await demand ops/s | async load demand ops/s | async load throughput gain | async load makespan reduction |
|---:|---:|---:|---:|---:|
| 2 | 6,128.257 | 7,952.771 | +29.77% | 22.94% |
| 4 | 11,476.680 | 15,508.576 | +35.13% | 26.00% |
| 8 | 21,470.680 | 27,527.730 | +28.21% | 22.00% |

每个 topology 的 submit/await 和 async load 都是 7/7 valid seeds；4-node 与 8-node 各有 14/14
有效 run。2-node cluster 行由同一个 scale reporter 从 formal acceptance JSONL 中重新
聚合，避免 nodeA 口径与 scale-out 口径混用。

### 5.2 Scale efficiency

以 2-node throughput 为基准：

| Path | 4-node speedup / ideal efficiency | 8-node speedup / ideal efficiency |
|---|---:|---:|
| submit/await demand | 1.8727× / 93.64% | 3.5036× / 87.59% |
| async load demand | 1.9501× / 97.50% | 3.4614× / 86.54% |

async load 在 2→4 节点的扩展效率更高；到 8 节点时两条路径都低于理想线性扩展。async load 的
绝对 throughput 仍更高，但 4→8 的边际 scale efficiency 为 88.75%，低于 submit/await 的
93.54%。这说明 async load 的优势没有随 node count 单调扩大，不能只看 4-node 的最高
`+35.13%` 就外推。

### 5.3 历史 CPU/elapsed 诊断字段

| Nodes | submit/await process CPU | async load process CPU | async load EL0 scheduler elapsed |
|---:|---:|---:|---:|
| 2 | 18.902 s | 14.731 s | 6.354 s |
| 4 | 37.641 s | 30.038 s | 13.211 s |
| 8 | 80.849 s | 66.490 s | 29.964 s |

async load 不需要额外 helper vCPU，QEMU 也不保存 guest coroutine context；但 guest EL0
scheduler 与 application coroutine 共用同一个 guest core。`el0_scheduler_ns` 记录
scheduler 区间的 elapsed time，其中可能包含等待 completion 的时间；它不能直接与
process CPU 相加。旧报告中的 “async load total vs submit/await” 因此撤回。schema v2 不使用这些字段
选择 sync、submit/await、async load；后续机制成本需要 scheduler active cycles 和 upcall/context-switch
cycles 才能独立核算。

## 6. Gate scenario 为什么是 10 ms，而性能点是 1 µs

async load phase gate 需要观测一个强时序事实：coroutine 0 的普通 `LDR` 进入 pending 后，
EL0 scheduler 必须切到 coroutine 1，并让 coroutine 1 在 coroutine 0 complete 前发出
自己的 `LDR`。在 100-µs gate latency 下，QEMU 的 host scheduling 抖动会让 complete
先于第二条 `LDR`，强 gate 会正确地 fail-closed。

因此五个 phase gate 使用
`scenarios/mvp_2host_async_load_remote_10ms.yaml`，为功能时序留出确定窗口。正式 evaluator
为每个 performance case 生成独立 model manifest；acceptance 的实际 timed case 仍是
1 µs，并没有把 10 ms 当成性能数据。gate scenario hash 与 case model hash 分开记录，
二者不能混为一谈。

## 7. 执行异常与处理

4-node seed 5 的第一次 submit/await 启动遇到 serial TCP port 被远端另一个隔离 cgroup 占用，
nodeB 在 QEMU 启动前退出。该尝试没有 summary/evidence，未计入结果；遗留的 nodeA/C/D
QEMU 按精确 PID 清理，失败日志放在 `attempts/`。重新随机选择空闲端口后，同一 seed
通过。最终 4/8-node 运行结束后均确认无残留 `qemu-system-aarch64`。

SSH executor 同时显式使用 `ControlMaster=no` 和 `ControlPath=none`，避免用户级 SSH
复用连接把一个失效 control socket 传播到后续 case。只有能证明发生在 dispatch 之前的
banner/connect 失败才允许重试；已下发但状态不明的 case 不自动重跑。

## 8. 2-node 7-seed coarse runtime policy

正式合并目录：

```text
out/obmm-remote-load/policy-coarse-7seed-20260817-r1/
```

四份 source campaign 分别覆盖 seed 1..3/4..7 和 coroutine 2/4/8/32。合并器验证
source 完整性、paired seed universe、主机分工和唯一 artifact fingerprint 后，得到
2,240/2,240 valid canonical run，`validation.status=pass`。13 份 source attempt 没有
进入 merged raw，quarantine 为 0。

policy schema v2 使用相同的 2,240 个 canonical raw 做离线重聚合，无需重新运行 QEMU。
选择条件为相同 `extra_vcpus`、paired median workload-makespan gain 至少 10%、paired
95% CI 下界至少 5%，以及 correctness/failure/duplicate/drain gate 通过。单次
load-to-resume p99 作为观测项保留。

| L | C | W | Transparent policy | Explicit policy |
|---:|---:|---:|---|---|
| 0/1/10 µs | 2/4/8/32 | 0/10/100/1000 µs | sync | sync |
| 100 µs | 2/4/8/32 | 0/10/100/1000 µs | async load | async load |
| 1000 µs | 2 | 0/10/100/1000 µs | async load | submit/await |
| 1000 µs | 4 | 0/10/100 µs | async load | submit/await |
| 1000 µs | 4 | 1000 µs | async load | async load |
| 1000 µs | 8/32 | 0/10/100/1000 µs | async load | async load |

完整选择表、单次 load latency 与 workload makespan 的口径、适用范围、机器可读文件和
后续 boundary refinement 见
[sync、submit/await、async load 运行时选择表](2026-08-17-obmm-runtime-policy-selection.md)。

### 8.1 Fine-grained formal boundary

screening 与 C/W tracing 使用 3 seeds 覆盖 224 个离散 bucket，从中识别 35 条相邻
latency winner 翻转，并选择翻转两侧 70 个 endpoint。formal matrix 对每个 endpoint
执行 sync、submit/await demand、submit/await lookahead 和 async load 四条路径、7 个 paired seed，共
1,960 个 run。n4-910c 完成 952 个，n4-910c1 完成 1,008 个；合并结果为
`validation.status=pass`、0 invalid、单一 artifact fingerprint、0 formal quarantine。

正式 endpoint 表明当前 QEMU PoC 的 sync/async load crossing 位于 30--75 µs 之间，L=50 µs
是依赖 C/W 的混合层。formal boundary 只验证 crossing 两侧的离散点，未覆盖的 bucket
仍回退 sync。完整表格和 SVG 见
[sync、submit/await、async load 运行时选择表](2026-08-17-obmm-runtime-policy-selection.md)。

## 9. Full matrix 的真实状态

完整矩阵 dry-run 目录：

```text
out/obmm-remote-load/p3-full-abi-v2-dry-run-20260813-r1/
```

它展开出 4,942 个 run：3,640 scalar、1,302 range；其中包含 latency/compute、
correctness、jitter/tail、duplicate、error 和 drop-timeout sweep。dry-run 目录只有
manifest 和 model documents，所以状态按设计为 `invalid dry-run`。

正式 campaign 曾在 `n4-910c` 的隔离工作区后台启动。r1/r2/r3 只保留为历史
证据；当前唯一可续跑的 campaign 是 r4：

```text
/home/ll/ub_sim_p2b_v2_20260812/out/obmm-remote-load/
  p3-full-abi-v2-20260813-r4/
```

2026-08-14 审计时，r4 evaluator PID `419618` 处于 `Tl`（SIGSTOP）状态，canonical
raw 为 `541/4,942`，`raw-attempts=3`，campaign 自身 QEMU 为 0。外部 QEMU 污染窗口
内的证据已移入 `raw-quarantine/`，不得进入正式聚合。用户已经明确要求暂停 P3，
所以即使主机当前空闲也不得自动恢复。它使用 `--local-repo` 直接在远端执行；每个
case 仍有独立外层 deadline、process-group cleanup 和 immutable raw JSONL。本文只有在
P3 被明确恢复、`OBMM_EVAL_COMPLETE` 出现、4,942 个 canonical raw run 完整聚合且
`validation.status=pass` 后，才会把 full matrix 改写为完成。

因此当前阶段的准确表述是：

- **P3 ABI v2 acceptance：完成；**
- **P3 2/4/8-node 基准点 scale-out：完成；**
- **P3 2-node coarse runtime policy：完成，已发布 measured-bucket 离线选择表；**
- **P3 full sensitivity / break-even campaign：已安全暂停，待明确恢复和最终聚合。**

coarse policy 只发布精确 measured bucket。full matrix 完成前，不发布 jitter、failure、
range、4/8-node 或未测 latency/compute/concurrency 区间的外推结论。

## 10. 复现入口

2-node formal evaluator：

```text
sim-cli obmm-remote-load-eval \
  --matrix scenarios/experiments/obmm_remote_load_eval_acceptance_v1.yaml \
  --scenario scenarios/mvp_2host_async_load_remote_10ms.yaml \
  --bands scalar,range \
  --seeds 1..7 \
  --gate-dir out/obmm-remote-load/gates-p3-abi-v2-10ms-20260813 \
  --remote-target n4-910c \
  --remote-repo /home/ll/ub_sim_p2b_v2_20260812 \
  --output-dir out/obmm-remote-load/p3-acceptance-abi-v2-20260813-r1
```

2/4/8-node raw evidence 的统一聚合入口：

```text
sim-cli obmm-remote-load-scale-report \
  --manifest <run-manifest.json> \
  --raw-dir <raw-evidence-dir> \
  --output-dir <scale-summary-dir> \
  --case-ids S1-submit-await-demand,S3-async-load-demand
```

7-seed policy 合并入口：

```text
sim-cli obmm-remote-load-policy-merge \
  --matrix scenarios/experiments/obmm_remote_load_policy_coarse_v1.yaml \
  --input <n4-low-c-seed1-3> \
  --input <n4-low-c-seed4-7> \
  --input <n4c1-high-c-seed1-3> \
  --input <n4c1-high-c-seed4-7> \
  --seeds 1..7 \
  --output-dir out/obmm-remote-load/policy-coarse-7seed-20260817-r1
```

细粒度 screening/tracing 完成后，使用 CLI 从相邻 latency winner flip 生成 formal
matrix；生成器拒绝覆盖已有输出：

```text
sim-cli obmm-remote-load-policy-boundary-select \
  --input <screening-low-c> \
  --input <screening-high-c> \
  --input <tracing-low-c> \
  --input <tracing-high-c> \
  --output-matrix scenarios/experiments/obmm_remote_load_policy_boundary_formal_v1.yaml \
  --output-report <selection.json>
```

formal source 使用相同的 `obmm-remote-load-policy-merge --seeds 1..7` 入口合并，矩阵
替换为 `obmm_remote_load_policy_boundary_formal_v1.yaml`。

`out/` 是生成物，不提交 Git；本报告只提交可复核的指标、hash、规则和结论边界。
