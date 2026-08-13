# P2B ABI v2 实现总结与源码导读

> 状态：ABI v2 的 2-node producer/consumer 功能目标已完成，并在 `n4-910c`
> 通过 ARM64 Linux 原生构建、远端 QEMU guest E2E 和机器可读 P2B phase gate。
> P3 ABI v2 的 2-node acceptance 与 4/8-node 定向 scale-out 已完成；4,942-case
> full matrix 尚待完成。P3 不是本次 P2B 功能验收的未完成项
>
> 日期：2026-08-12
>
> 详细设计：[P2B：普通 `LDR` + 自定义 EL0 upcall + EL0 协程调度核心](p2b-scheduler-core-detailed-design.md)
>
> 可视化：[P2B core mechanism 与 guest EL0 scheduler 边界](p2b-scheduler-core-flow.svg)

## 1. 结论

P2B 已按目标边界重构为：普通 AArch64 EL0 `LDR` 遇到 registered remote mapping
且不能同步完成时，QEMU 保持原 load 未退休，只把执行流直接导向 registered EL0
upcall entry。guest EL0 runtime 自己保存被中断 coroutine 的完整上下文、维护
`READY/RUNNING/WAIT_REMOTE/FAULTED/DONE`、选择另一个 coroutine，并请求 core 原子
安装自己选中的 context image。completion 到达后，EL0 runtime patch 挂起 context 的
`Rt` 和 `PC=fault_pc+4`，再择机恢复。

这是一套故意新增的、非标准 Arm core 语义。它不是 Linux signal，也不需要先进入
EL1 exception vector。QEMU 提供的是两项 mechanism：

1. 在精确 EL0 instruction boundary 直接改 `PC` 到 upcall entry，其他寄存器不变；
2. 执行自定义 `HLT #0x5343` 时，原子安装 EL0 选中的完整 context image。

QEMU 不再拥有 coroutine Context Store、ready queue、状态机或 round-robin policy。
这些职责现在都在 `guest-linux/aarch64/libs/obmm_scc/`。

![P2B EL0 upcall 与 EL0 scheduler](p2b-scheduler-core-flow.svg)

## 2. Ownership 已落到代码

| 能力 | QEMU/core mechanism | guest EL0 runtime |
|---|---:|---:|
| 识别 registered remote scalar `LDR` | 是 | 否 |
| 保持 load 未退休、维护 PLT | 是 | 否 |
| 提交 provider-neutral remote read | 是 | 否 |
| pending/completion direct EL0 upcall | 是 | 接收 |
| 保存 x0..x30/SP/PC/NZCV/Q/FP/TLS | 否 | 是 |
| coroutine Context Store | 否 | 是 |
| READY/WAIT/FAULT/DONE 状态机 | 否 | 是 |
| 选择下一个 coroutine | 否 | 是 |
| completion patch `Rt/PC` | 否 | 是 |
| 原子安装已选 context | 是 | 发起 |

代码层有两个反向约束：

- QEMU public SCC model 不存在 `ObmmSccArchState`、`context_create()` 或
  `schedule_next()`；
- contract test 要求这些符号不得重新进入 QEMU，同时要求 guest library 必须出现
  ready/wait 状态、选择函数和 `Rt/PC` patch。

## 3. 端到端时序

### 3.1 Pending

1. coroutine A 执行普通 `LDR X3, [remote]`；
2. TCG helper 先做 owner、range 和标准 MMU permission 检查；
3. QEMU 分配 PLT 并通过 P1 backend 提交 remote read；
4. QEMU enqueue `PENDING(A, token, fault_pc, Rt=X3)`；
5. helper 在正常 `qemu_ld` 之前退出 TB，所以原 load 未执行，`X3` 和 writeback 均未提交；
6. QEMU 保留所有 EL0 register，只令 `PC=obmm_scc_upcall_entry`；
7. EL0 assembly 在 A 的 stack 上保存 832-byte transient frame；
8. assembly 切到独立 scheduler stack；
9. EL0 C dispatcher 把 frame 复制到 A 的 Context Store，将 A 置为 `WAIT_REMOTE`；
10. EL0 round-robin 选择 B，并以 `HLT #0x5343` 请求 core 安装 B。

### 3.2 Completion

1. P1 completion sink 校验 PLT token/generation，enqueue `COMPLETE(A, value)`；
2. 若 B 正在运行，QEMU 在下一个 EL0 TB boundary 直接 upcall；
3. EL0 assembly 保存 B；B 的 event `interrupted_pc` 成为其保存 PC；
4. EL0 dispatcher 找到等待的 A，校验 context ID 和 PLT token；
5. `Rt != 31` 时写 `A.context.x[Rt]=value`；
6. 设置 `A.context.pc=fault_pc+4`，令 A 变 `READY`；
7. EL0 policy 决定下一 context；A 被选中时从原 `LDR` 下一条指令继续。

若没有 READY coroutine，EL0 scheduler 保持在自己的 event wait loop；completion
到达后由 `GET_EVENT|WAIT` 取得，不恢复 A 重发 load。

## 4. 分层实现清单

### 4.1 QEMU：PLT 和 event，不再调度 coroutine

文件：

- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_scc.h`
- `vendor/qemu_8.2.0_ub/hw/ub/ub_scc.c`

`ObmmScc` 现在只维护：

- bounded PLT；
- slot/owner/generation token；
- pending/completion/fault event queue；
- scalar value endian assembly；
- stale/duplicate/capacity/overflow/fail-stop counters。

`obmm_scc_load_pending()` 只建 PLT 并产生 PENDING event；
`obmm_scc_load_complete()` 只产生 COMPLETE/FAULT event；
`obmm_scc_event_pop()` 在 terminal event 交付时回收 PLT。QEMU 的 context save/restore/
switch/bytes counters 在 ABI v2 必须为 0。

event queue 采用一条与未退休 load 直接相关的优先级规则：**当前刚触发 load 的
PENDING 必须先于队列中较早到达的 COMPLETE/FAULT 交付**。否则当前 coroutine 可能
先处理别人的 completion，却没有收到自己的 PENDING，随后被错误地当成可运行状态。
PENDING 从队首入队，terminal event 保持 FIFO；event `sequence` 在出队交付时分配，
因此 EL0 看到的 sequence 仍严格单调。

model spec 同步升级为：

```text
v2|enabled=1|contexts=64|pending=64|events=128|clock_mhz=2000
```

旧 `save/schedule/restore/commit cycles` 已从 scenario 和 QEMU config 删除，因为这些
动作不再由 QEMU scheduler model 执行。

### 4.2 QEMU device：direct event delivery 与 active context identity

文件：

- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_scc_device.h`
- `vendor/qemu_8.2.0_ub/hw/ub/ub_scc_device.c`

新增/保留的核心状态：

- `upcall_entry`；
- `active_context_id`；
- `logical_context_count`；
- `upcall_active`；
- 当前 delivered event；
- registered remote maps、PLT futures 和 P1 backend。

`ub_scc_cpu_take_upcall()` 只复制一个 event、填 `interrupted_pc`、设置
`upcall_active` 并返回 upcall entry。`ub_scc_cpu_resume()` 只验证 generation、home CPU、
slot 和嵌套状态，然后接受 EL0 提交的 context ID；它不扫描 context，也不选择 next。

event delivery 有两个入口：运行中的 coroutine 被 pending/completion 打断时使用 direct
upcall；所有 coroutine 都在 `WAIT_REMOTE` 时，EL0 scheduler 用 `GET_EVENT(WAIT)` 等待
并从 event queue 拉取 completion。第二条路径没有 active upcall frame，QEMU event
promote command 只要求 session active、event ready 且 delivered slot 为空。

新增 MMIO register 覆盖 upcall entry、logical context count、event metadata 和 event
ack/promote command。completion callback 只更新 PLT/event 并 kick home CPU，不写 guest
GPR。

### 4.3 AArch64 TCG：PC-only upcall 与原子 resume

文件：

- `vendor/qemu_8.2.0_ub/target/arm/helper.h`
- `vendor/qemu_8.2.0_ub/target/arm/tcg/translate-a64.c`
- `vendor/qemu_8.2.0_ub/target/arm/tcg/helper-a64.c`

普通 unsigned scalar load lowering 仍在正常 load 前调用 remote helper。pending 时 helper：

```text
env->pc = registered_upcall_entry
cpu_loop_exit_noexc()
```

它不复制 `CPUARMState`，也不安装别的 coroutine。每个 active EL0 TB 开头的 boundary
helper用于交付已到达的 completion event，动作仍然只有 PC redirection。

`trans_HLT()` 新增 `#0x5343` 分支。resume helper 从 guest 指针读取 832-byte context，
验证 session/owner/alignment/context identity 后一次性安装：

- `x0..x30`、`SP_EL0`、`PC`、NZCV；
- Q0..Q31、FPCR、FPSR；
- `TPIDR_EL0`；
- 清 exclusive monitor，重建 Arm hflags。

普通 `BR` 无法在不牺牲 scratch GPR 的情况下恢复任意保存点的全部 GPR，所以自定义
resume instruction 是这套模拟 core ABI 的必要部分，不是调度策略。

### 4.4 Guest UAPI 与 driver

文件：

- `guest-linux/kernel_ub/include/uapi/ub/obmm_scc.h`
- `guest-linux/aarch64/driver/linqu_ub_drv.c`

UAPI 从 v1 升级到 v2：

- 新增固定 832-byte `obmm_scc_context_v2` layout；
- `START` 新增 `upcall_entry` 和 `logical_contexts`；
- 新增 `obmm_scc_event_v2` 与 `GET_EVENT`；
- capability 明确 `DIRECT_EL0_UPCALL`、`EL0_RESUME`、`FULL_CONTEXT`；
- 删除 QEMU-owned create/destroy context、fault-context、context-exit ioctls。

driver 仍负责 owner TGID、single-CPU affinity、TTBR0、mapping fd/VMA/mem_id 交叉验证和
MMIO publish。direct event 已由 QEMU 送入 EL0 后，handler 用 `GET_EVENT` 取 payload 并
ack；无 READY coroutine 时，WAIT 模式轮询/睡眠，等待 QEMU event queue 后 promote。
非 WAIT 的 `GET_EVENT` 必须处于 active upcall；`GET_EVENT(WAIT)` 则只要求调用者仍是
session owner。这一区分避免 scheduler 在全部 coroutine 阻塞时错误收到 `-EPERM`。

### 4.5 Guest EL0 scheduler runtime

文件：

- `guest-linux/aarch64/libs/obmm_scc/obmm_scc.h`
- `guest-linux/aarch64/libs/obmm_scc/obmm_scc.c`
- `guest-linux/aarch64/libs/obmm_scc/obmm_scc_aarch64.S`
- `guest-linux/aarch64/libs/obmm_scc/Makefile`

这是重构的核心。C runtime 现在拥有：

- 每 coroutine 双 guard-page stack；
- user-space `obmm_scc_context_v2` Context Store；
- `FREE/READY/RUNNING/WAIT_REMOTE/FAULTED/DONE`；
- waiting PLT token；
- round-robin selector；
- completion `Rt/PC` patch；
- 独立 scheduler stack；
- `sigsetjmp/siglongjmp` run-return context；
- EL0 save/restore/switch/bytes/upcall/no-ready/scheduler-time metrics。

AArch64 assembly 在调用 C 之前保存全部 v2 context 字段。反汇编确认 resume instruction
为：

```text
d44a6860    hlt #0x5343
```

initial context 把 local descriptor 放在 `x19`，PC 指向 assembly bootstrap。entry return
后仍在 EL0 标记 `DONE`、切到 scheduler stack、选择下一 context；不再通过 ioctl 让 QEMU
替它处理生命周期。

### 4.6 Workload、scenario、evidence gate

文件：

- `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c`
- `crates/sim-config/src/lib.rs`
- `crates/sim-cli/src/obmm_remote.rs`
- `crates/sim-cli/src/obmm_eval.rs`
- `scenarios/mvp_*host_*.yaml`

workload 的数据面仍是普通 volatile 1/2/4/8-byte scalar load，没有 per-load submit/await。
旧 fault-service coroutine 已删除；FAULT event 由 EL0 scheduler 直接把目标 coroutine 置
为 `FAULTED`，并计入 failure/timeout。

2-node 功能验收新增 `--p2b-producer-consumer --producer-index 0`：nodeA export 并写入
每 coroutine 对应的确定值，nodeB import 同一个 export，内置两个 EL0 coroutine 和
scheduler core。runtime trace callback 在内存中记录 context、LDR、upcall 和 resume
顺序，运行结束后统一输出，避免串口 `printf` 改变短延迟调度时序。

新增 evidence：

- `el0_upcalls_pending/complete/fault`；
- `el0_context_saves/restores/switches/bytes`；
- `el0_scheduler_ns`、`el0_no_ready_waits`；
- `direct_el0_upcalls`；
- `qemu_context_saves/restores/switches/bytes`。

新 P2B gate 要求 EL0 指标非零、pending/complete 成对、save 等于 direct-upcall 数；同时
要求 QEMU context 指标及旧 scheduler-cycle 指标全部为 0。P2B 不再声明额外 helper
vCPU，EL0 scheduler 与 worker 在同一 home vCPU 上交替执行。producer/consumer gate
还逐 coroutine 验证 write/import/issue/pending/complete/resume/retire 的 context、token、
PC、offset 和 value，并要求至少一次“另一 coroutine 在当前 pending/complete 窗口内
实际发出 LDR”。

## 5. ABI v2 context layout

| offset | bytes | 内容 |
|---:|---:|---|
| 0 | 8 | context ID |
| 8 | 8 | flags |
| 16 | 248 | x0..x30 |
| 264 | 8 | SP_EL0 |
| 272 | 8 | PC |
| 280 | 8 | NZCV |
| 288 | 512 | Q0..Q31 |
| 800 | 8 | FPCR |
| 808 | 8 | FPSR |
| 816 | 8 | TPIDR_EL0 |
| 824 | 8 | reserved |

总大小 832 bytes，context pointer 要求 16-byte alignment。kernel static asserts、EL0
library static asserts 和 QEMU build-time asserts 使用同一组 offset。

## 6. 构建与自动化验证结果

| 验证 | 结果 |
|---|---|
| AArch64 `libobmm_scc` 交叉编译 | 通过，`-Wall -Wextra -Werror` |
| AArch64 workload 静态交叉链接 | 通过 |
| assembly 反汇编 | 通过，保存 GPR/SIMD，resume 为 `hlt #0x5343` |
| 本地 QEMU `qemu-system-aarch64` 增量构建 | 通过 |
| QEMU OBMM tests | SCC 7/7、model 7/7、backend 6/6，共 20/20 通过 |
| guest OBMM focused contracts | 27/27 通过 |
| Rust P2B phase-gate focused tests | 12/12 通过 |
| `sim-config` tests | 7/7 通过 |
| ARM64 Linux 原生 QEMU build | 在 `n4-910c` 通过，同一组 QEMU tests 20/20 通过 |
| ARM64 Linux kernel/driver/initramfs | 当前 kernel Image、`linqu_ub_drv.ko`、ABI v2 initramfs 均构建通过 |

QEMU unit tests覆盖 model spec v2、context ID、logical ordinal、pending/completion event、
PENDING 优先交付、1/2/4/8-byte endian value、fault/stale 和 capacity/fail-stop。

## 7. 远端 2-node QEMU guest 端到端结果

验证在 `n4-910c` 的隔离工作区
`/home/ll/ub_sim_p2b_v2_20260812` 执行。nodeA 的 test program A export 2 MiB 并写入
两个不同值；nodeB 的 test program B import 同一个 `export_mem_id`，内置两个 coroutine
和 EL0 scheduler core。两个 coroutine 分别执行一次普通 `LDR`，没有调用 submit/await。

![P2B 2-node producer/consumer 验收时序](p2b-2node-producer-consumer-validation.svg)

证据文件：

- 原始运行日志：
  `/home/ll/ub_sim_p2b_v2_20260812/out/p2b_v2_remote_validation/p2b-v2-producer-consumer-20260812-r15.log`；
- machine gate：
  `/home/ll/ub_sim_p2b_v2_20260812/out/p2b_v2_remote_validation/gates/2node-producer-consumer-r15/p2b.json`；
- gate 结果：`schema=1 phase=p2b runs=1 status=pass`；
- harness 清理结果：`qemu_destroyed=1`，验证后无残留 QEMU process。

旧 r8 是 nodeA/nodeB 对称 workload smoke，不包含明确的 producer/consumer ownership，
也没有要求 coroutine 1 在 coroutine 0 completion 前实际发出 `LDR`，因此不再作为当前
P2B 功能验收证据。

### 7.1 产物绑定

| 产物 | SHA-256 / contract |
|---|---|
| QEMU | `362e7745d3fa6e55bdbdb6f33438ef2a224c64d82061a0da14d7ce3325b2958c` |
| kernel Image | `8f187f08ba0c28260ab5b6267f8dfeeee0e229938755b36e42596f684b25ccbb` |
| initramfs | `4cc0642a1b15daa607956c63ffd94af09dcd3409dd132270f27b7838771c4c32` |
| `linqu_ub_drv.ko` | `7f0f576493fb1783e2a0b82fb3e5a5790c7652cfb669c91da497636f06ce97a8` |
| scenario file | `636feccb702d884f8c30a15d689cd11582ec3d3b5e776532a0b14d3986532837` |
| phase-gate scenario contract | `fnv1a64:3ced9932a5444d6f` |
| remote model file | `e8d7d2e291a9612e1d8b95f78ddee56069d22bc3b4b0256cc3fb6b8cec271f04` |
| model contract | `fnv1a64:e0b3f5ef7cc0da5c` |

`scenarios/mvp_2host_p2b_remote_10ms.yaml` 使用 fixed 10 ms、无 jitter/drop/error/
duplicate。它只为稳定制造可观察 overlap，不是性能模型。

### 7.2 跨节点和逐 coroutine 结果

| 证据 | 结果 |
|---|---|
| nodeA write 0 | offset `0x1000`，value `4d54ca036b700e61` |
| nodeA write 1 | offset `0x2000`，value `4d54ca036b700e60` |
| nodeB import | `source_export_mem_id=1`，与 nodeA export 相同 |
| coroutine 0 | pending=1，complete=1，actual=`4d54ca036b700e61`，pass |
| coroutine 1 | pending=1，complete=1，actual=`4d54ca036b700e60`，pass |
| overlap | `pending(c0) < resume(c1) < LDR-issue(c1) < complete(c0)` |
| scheduler ownership | EL0 saves/restores/switches = 2/4/3；QEMU context counters 全 0 |
| final state | SCC/backend pending = 0/0；trace dropped=0；QEMU destroyed=1 |

`OBMM_P2B_CAUSAL_SUMMARY blocked_load_switches=1 status=pass` 不是根据 counter 推断，
而是 runner 扫描逐事件日志后得到：另一个 context 的 resume 和另一个 coroutine 的
`LDR issue` 都必须落在某个 pending/complete 开区间内。Rust phase gate重新执行相同
检查，并交叉验证 context ID、token、PC、offset 和 value。

### 7.3 只有真实 E2E 才暴露出的边界问题

| 问题 | 根因 | 修复 |
|---|---|---|
| 832-byte context 跨 4-KiB 页触发 QEMU assertion | 把跨页 frame 一次交给只接受单页范围的 `probe_access()` | 按 guest page 边界分段 probe，并传递真实 TCG return address |
| EL0 选出的下一个 coroutine 被 QEMU 拒绝 | resume 路径残留 QEMU-owned active-context policy | QEMU 仅校验 session/home CPU/generation/slot，不再限制 EL0 的调度选择 |
| coroutine 结束附近收到合法 completion 后 `_exit(127)` | upcall dispatcher 只接受 `RUNNING`，错误拒绝 `DONE` transition | 接受 `RUNNING/DONE`；保存真实 frame，但不把已结束 context 重新置为 READY |
| 偶发少完成一次或虚假 clock regression | terminal event 抢在当前 load 的 PENDING 前交付；OS-thread TLS 时钟被多个用户态 coroutine 共享 | PENDING 优先、出队分配 sequence；时钟 watermark 改为 per-coroutine |
| 两个 load 都 pending 后 scheduler 返回 `-EPERM` | driver/QEMU 把同步 `GET_EVENT(WAIT)` 错误要求为 `upcall_active` | direct event 与 no-ready synchronous pull 分离；WAIT 只要求 active owner session |

## 8. 功能验收结论、下一阶段与旧证据

2-node ABI v2 producer/consumer 功能验收已经完成。P3 acceptance 与定向 scale-out
也已生成新证据；故障硬化和额外指令/pattern 覆盖可单独扩展。它们不是当前 P2B
完成条件。当前进度为：

1. 使用新 run ID 完成 2-node、7-seed `S3-p2b-demand` 和完整 P3 acceptance（完成）；
2. 扩展 4/8-node 定向 scale-out，每个 topology 14/14 valid runs（完成）；
3. 执行 4,942-case 性能 campaign 与 break-even 分析（待完成）；
4. 另行执行 timeout、stale、duplicate、event overflow、invalid resume fault-injection
   gate。

旧 `qemu-internal-scc` 的 49-case/gate 结果验证的是 QEMU 保存和选择 context 的另一套
架构，不能作为 ABI v2 的证据。当前 P2B 状态必须准确写为：

```text
ABI v2 2-node producer/consumer functional acceptance passed
```

不得把“full matrix 尚未执行完毕”写成 P2B 功能仍未完成；也不得因此把 P3 降级成
可选工作。当前结果见
[2026-08-13 P3 性能评估](2026-08-13-obmm-p3-performance-evaluation.md)，它没有复用
当前 2-node 功能 gate 代替性能数据。

## 9. 当前限制

- 一个 Linux process、一个固定 home vCPU；
- 最多 64 coroutine、64 pending loads、128 events；
- 每 coroutine 最多一个 unresolved remote load；
- unsigned scalar `LDRB/LDRH/LDR Wt/LDR Xt`，1/2/4/8 bytes；
- 不支持 signed、store、atomic/exclusive、acquire、pair、SIMD memory op、SVE/SME；
- custom upcall 不等价于标准 Arm exception；
- `HLT #0x5343` 只在 active owner-matched EL0 SCC session 中有效；
- worker 不应依赖 signal handler 在 upcall-active 窗口做 context switching；
- 当前是 demand-pending，不包含 P2A 的 pre-submit schedule-ahead 窗口。
