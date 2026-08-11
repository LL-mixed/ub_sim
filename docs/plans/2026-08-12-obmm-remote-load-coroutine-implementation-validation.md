# OBMM 远端 Load 协程 P0–P4 实施与验证报告

> 状态：实现完成；P0/P1/P2A/P2B/P4 gate 通过；P3 49-case 正式验收通过
>
> 日期：2026-08-12
>
> 上位设计：[OBMM 远端 Load 协程可行性与验证设计](2026-08-11-obmm-remote-load-coroutine-feasibility-design.md)

## 1. 结论与边界

P0、P1、P2A、P2B、P3 和 P4 已在同一条 provider-neutral OBMM read 底座上实现。
正式验收覆盖 7 条 canonical path、7 个 seed，共 49 次运行；49 次均通过，且所有
raw evidence 绑定同一 QEMU binary SHA-256、scenario hash、model contract hash 和
operation-list hash。

这证明以下链路已闭环：

- P0 的模型与同步基线能够形成可复现证据；
- P1 能同时服务 test、P2A 和 P2B sink，并处理 64 in-flight 与 terminal race；
- P2A 能在同一 guest vCPU 上执行 `A await → B 推进 → A 恢复`；
- P2B 能在普通 `LDR` 未退休时保存 context，经 scheduler core 选择其他 context，
  最终把 value 或 fault 精确提交到原 load；
- P4 能以标准 `userfaultfd` MISSING 路径提供 4-KiB 透明页访问对照；
- P3 对 scalar、range 和 transparency 分带汇总，invalid evidence 不进入统计。

49-case 验收是实现/证据链验收，不是完整性能研究。完整矩阵可确定性展开为 4,942
个 case，但目前只完成 dry-run；在所有延迟、jitter、failure、并发度、lookahead 和
compute 因子点都正式执行前，不应从单个 1 µs acceptance 点外推 break-even 结论。

![P0–P4 实施与验收状态](obmm-remote-load-implementation-status.svg)

## 2. 阶段结果

| 阶段 | 实现结果 | 可执行产物 | 验收结果 |
|---|---|---|---|
| P0 | strong scenario schema、四类同步基线、确定性 latency/jitter/failure 模型、三类时钟与 hash 闭环 | `obmm-remote-baseline`、`obmm-remote-phase-gate --phase p0` | pass |
| P1 | 64 parent、child aggregation、bounded result ownership、generation-safe sink、timeout/cancel/retire/capacity | `obmm-remote-conformance`、QEMU unit model | 144/144 conformance pass；gate pass |
| P2A | 独立 async endpoint、64-byte SQ/CQ、registered buffer、future、AArch64 EL0 stackful coroutine | `obmm_async_coroutine --mode async-poll` | 2/4/8-node gate smoke pass；正式 7 seeds pass |
| P2B | RLA、PLT、Context Store、SCC、TCG scalar-load hook、precise commit/fault | `obmm_async_coroutine --mode scheduler-core` | 2/4/8-node gate smoke pass；正式 7 seeds pass |
| P3 | matrix expansion、远端 executor、raw evidence、fail-closed gate、统计与报告 | `obmm-remote-load-eval` | 49/49 acceptance pass；4,942-case full matrix dry-run pass |
| P4 | UFFD MISSING、OBMM source/shadow range、handler vCPU、page 状态机与 phase metrics | `obmm_async_coroutine --mode userfaultfd` | 2/4/8-node gate smoke pass；正式 7 seeds pass |

## 3. 实现说明

### P0：同步基线与延迟模型

`crates/sim-config` 增加了 remote-memory model 的强类型 schema；2/3/4/8-host scenario
均显式声明该模型。`crates/sim-qemu/src/obmm_remote_model.rs` 与 QEMU
`hw/ub/ub_obmm_remote_model.c` 使用同一 operation identity 和确定性决策规则，避免
host 调度顺序改变 latency、jitter、drop/error 或 duplicate 选择。

`crates/sim-cli/src/obmm_remote.rs` 提供四类 canonical baseline、manifest 生成和
phase gate。P0 gate 要求 case-specific model 语义、payload checksum、scenario hash
与 model contract hash 同时一致，不能只凭进程退出码判定通过。

### P1：provider-neutral split-phase backend

QEMU `hw/ub/ub_obmm_remote.c` 实现 parent/child lifecycle、64 项容量、result pool 和
exactly-once terminal delivery；`hw/ub/ub_ubc.c` 把 SIM_DEC/OBMM response 路由到
test、P2A 或 P2B adapter。provider 不持有 guest pointer，也不理解 SQ/CQ、future、
PLT 或目标寄存器。

P1 conformance matrix 包含 3 种 sink、8 种状态/竞态 case 和 P2A/P2B 各自允许的
访问粒度，共 144 个 case；覆盖 inline、64 in-flight、reorder、duplicate、timeout、
cancel race、retire 和 capacity full。

### P2A：显式 submit/await + EL0 协程

driver 中的独立 async endpoint 管理 queue ownership、destination registration、mmap、
poll/IRQ 和 generation；`guest-linux/aarch64/libs/obmm_async/` 管理 SQ/CQ、future 与
AArch64 stackful context。共同 workload 位于
`guest-linux/aarch64/apps/obmm_async_coroutine/`。

`submit` 退休后，应用可以继续运行；只有 `await` 发现 future 尚未 terminal 时才切换
协程。结果先落入 P1-owned result，再由 P2A sink 复制到 registered destination，最后
release-publish CQE。该顺序避免 stale completion 写入已复用的 destination。

### P2B：普通 `LDR` + dedicated scheduler core

QEMU `target/arm/tcg/translate-a64.c` 和 `helper-a64.c` 只对白名单 OBMM scalar load
插入 RLA hook。`hw/ub/ub_scc.c` 与 `ub_scc_device.c` 管理 PLT、Context Store、
ready/wait/fault 状态及 scheduler cost model；guest 的一次性 control plane 位于
`guest-linux/aarch64/libs/obmm_scc/`。

pending 时原 `LDR` 不退休，`PC=P`、`Rt=old` 被保存；completion 到达后只允许一次
commit，成功写 `Rt` 并推进 `PC`，失败则生成 precise fault。TCG vCPU thread 对 SCC/
backend 的访问使用 QEMU iothread lock，与 I/O thread 的 response/timer completion
串行化；TX ring writer 也独立加锁。两项修复消除了压力下的 frame loss 和偶发 stale
payload。

### P3：对比评估与证据链

`crates/sim-cli/src/obmm_eval.rs` 实现 strict matrix parser、deterministic expansion、
远端执行、per-case timeout、process-group 清理、pre-dispatch SSH 限定重试、raw JSONL、
invalid gate、bootstrap confidence interval 和 Band S/R/T 报告。

评估器 fail closed：dry-run、缺 gate、seed 不足、artifact hash 混用、summary 缺失或
重复、checksum/operation identity 不一致、case timeout 和非零退出都不能进入统计；
已有 output directory 和 raw file 不会被静默覆盖。

完整矩阵定义在 `scenarios/experiments/obmm_remote_load_eval_v1.yaml`；正式 acceptance
子集定义在 `obmm_remote_load_eval_acceptance_v1.yaml`。后者保留 7 seeds、操作数、
持续时间和证据要求，但每条 canonical path 只选择一个 factor point。

### P4：标准 userfaultfd MISSING 基线

`guest-linux/aarch64/common/obmm_uffd.*` 封装 capability 和 ioctl；共同 workload 的
`uffd_mode.c` 与 `uffd_state.c` 分离 OBMM source range、anonymous shadow range、handler
stack 和 staging buffer，并记录 fault/read/copy/wake 各阶段。

remote read 失败时页面进入 `POISONED` 或 `FAIL_STOP`，不会用 zeropage 把失败伪装成
成功。P4 保持“faulting Linux thread 在内核等待”的标准语义，因此需要额外 handler
thread/vCPU，不声称实现同一 kernel thread 内的 EL0 协程切换。

## 4. 正式验收证据

正式运行目录为 `out/obmm-remote-load/p3-acceptance-v6/`；该目录是生成物，不提交到
源码仓库。关键证据如下：

| 字段 | 值 |
|---|---|
| 状态 | `pass` |
| 正式运行 | 49 valid / 0 invalid |
| canonical path | S0、S1、S2、S3、R0、R1、R2 |
| seeds | 1–7 |
| matrix hash | `fnv1a64:edfbaf665424997b` |
| scenario hash | `fnv1a64:11efb67bb0d07d6c` |
| model contract hash | `fnv1a64:54431162a1abe3be` |
| topology | 2 hosts；`fnv1a64:566c7ce6174b0fe1` |
| QEMU binary SHA-256 | `27109cceda2ebe9f5a537f8a9419d01f98ac25083d3da742e052531b981884ac` |

`validation.json` 记录五个 phase gate 全部为 `pass`；49 份 raw JSONL 均退出 0，未出现
`OBMM_VERIFY_FAILURE` 或 SSH retry。`summary/scalar.csv`、`range.csv`、
`transparency.csv` 和 `break-even.csv` 只聚合 valid rows。

## 5. 测试与环境审计

| 验证 | 结果 | 说明 |
|---|---|---|
| P3 Rust focused tests | 19 passed | parser、hash、gate、timeout、process group、SSH retry、aggregation |
| guest OBMM contracts | 25 passed | async、SCC、UFFD build/UAPI/script contracts |
| QEMU OBMM/SCC unit tests | 26 passed | remote model、split-phase backend、scheduler core |
| 正式 P3 acceptance | 49 passed | 7 paths × 7 seeds |
| remote `cargo test --workspace` | OBMM tests passed；full suite blocked | 远端只有 GCC 13；既有 Simpler native tests 明确要求真实 `g++-15` |
| remote Python discovery | OBMM focused tests passed；full suite blocked | 远端快照缺既有 W5 scripts、`/private/tmp` 与 zsh loadable modules |

受环境阻塞的 full-suite 结果不能记为通过，也不改变 OBMM focused、QEMU unit 和正式
49-case 验收的结果。关键 Simpler backend parity 用例已在具备真实 `g++-15` 的本机
单独通过；没有用软链或 GCC 13 冒充固定 ABI 工具链。

## 6. 复现入口

完整矩阵只做展开和 gate 审计：

```text
cargo run -p sim-cli -- obmm-remote-load-eval \
  --matrix scenarios/experiments/obmm_remote_load_eval_v1.yaml \
  --scenario scenarios/mvp_2host_single_domain.yaml \
  --seeds 1..7 \
  --output-dir out/obmm-remote-load/p3-dry-run \
  --dry-run
```

正式 acceptance 必须在允许启动 QEMU 的远端环境运行：

```text
cargo run -p sim-cli -- obmm-remote-load-eval \
  --matrix scenarios/experiments/obmm_remote_load_eval_acceptance_v1.yaml \
  --scenario scenarios/mvp_2host_single_domain.yaml \
  --seeds 1..7 \
  --output-dir out/obmm-remote-load/p3-acceptance \
  --remote-target <host> \
  --remote-repo <repo>
```

正式执行前必须先生成并通过 P0/P1/P2A/P2B/P4 gate；`--dry-run` 结果始终标记为
invalid evidence，不能被 aggregate-only 重新解释为正式测量。

## 7. 剩余工作不是实现缺口

- 在具备真实 `g++-15`、完整 W5 scripts 和 zsh modules 的标准远端镜像重跑整个仓库
  full suite；
- 执行 4,942-case 全因子性能 campaign，形成不同 latency/compute/concurrency 区间的
  break-even 结论；
- 若面向真实硬件，需把 QEMU 的自定义 P2B architecture 落到目标 core ISA、Context
  Store 容量、coherence 和 interrupt/debug 规范；当前结果只证明模拟设计可实现。
