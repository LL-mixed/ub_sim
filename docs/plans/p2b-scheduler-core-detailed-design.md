# P2B：普通 `LDR` + dedicated scheduler core 详细设计

> 状态：已实现；P2B gate 通过
>
> 日期：2026-08-11
>
> 上位设计：[OBMM 远端内存 Load 的 EL0 协程延迟隐藏](2026-08-11-obmm-remote-load-coroutine-feasibility-design.md)
>
> 共同底座：[P1：provider-neutral split-phase backend](p1-split-phase-backend-detailed-design.md)
>
> 软件基线：[P2A：submit/await + EL0 协程详细设计](p2a-submit-await-detailed-design.md)
>
> 实施证据：[P0–P4 实施与验证报告](2026-08-12-obmm-remote-load-coroutine-implementation-validation.md)

## 1. 结论

P2B 保留应用数据面上的普通 AArch64 `LDR`，但代价不是“少一个 API”这么简单：
application core 必须支持一个 load 未退休时换出完整 EL0 context；dedicated scheduler
core 必须拥有 Context Store、pending-load table 和 ready/wait 状态；completion 必须把
结果精确提交到原 load 的目标寄存器，或把 context 置为显式 fault 状态。

P2B 不是 Arm exception、Linux signal，也不是 QEMU 把 PC/SP 改到 EL0 trampoline。
它是一套自定义 core architecture，QEMU 负责模拟硬件事件、状态容量和周期成本。

对用户的直接影响是：每次 load 不需要 `submit/await`，已有数据访问表达可保持；但
程序仍需一次性的 control-plane ABI 创建 coroutine context、注册允许 interception 的
OBMM map、注册 fault-service context。MVP 还限制 signal/debugger、blocking syscall、
原子/向量/acquire load，因此不能宣称任意未修改程序透明运行。

![P2B 未退休 LDR、Context Store 与 scheduler core](p2b-scheduler-core-flow.svg)

## 2. V1 验证边界

### 2.1 固定拓扑

- 每个 VM/node 先启用 1 个 application core；
- 配置 1 个 dedicated scheduler core 实例，只服务该 application core；
- 最多 64 个 registered coroutine contexts；
- pending-load table（PLT）64 项；
- 每个 context 最多 1 个 unresolved remote load；
- scheduler core 是独立的、带 event queue 和周期成本的模拟硬件资源，不占用
  application core 的 EL0 instruction stream，也不等同于 QEMU host thread。

多 application core 共享一个 scheduler core 的仲裁、公平性和 NUMA 放在 V2；V1
先证明 precise load/context 语义，避免把扩展性问题混入正确性验证。

### 2.2 支持的 load

V1 仅支持 AArch64 EL0、天然对齐、单寄存器、normal-memory scalar load：

- `LDRB/LDRH/LDR Wt/LDR Xt` 和对应 unscaled immediate/register-offset 形式；
- 1/2/4/8 bytes；
- zero-extend；sign-extend 仅在 `extend_kind` golden tests 完成后开启；
- `Rt == XZR/WZR` 允许，仍执行访问和 fault，但不写 GPR。

以下指令不 interception，若地址落入只允许 async 的实验 map 则 fail closed：

- `LDAR/LDAPR`、barrier-coupled load；
- `LDXR/LDAXR`、CAS、FAA 等 exclusive/atomic；
- pair、SIMD/FP、SVE/SME、MTE 特殊访问；
- unaligned、跨页、device/strongly-ordered access；
- AArch32/Thumb。

### 2.3 OS/runtime 限制

所有 context 属于同一 Linux process、address space、home vCPU 和 owner generation。
应用在进入 `scc_run()` 前固定 CPU affinity。V1：

- 允许不阻塞的普通函数和计算；
- worker context 不执行可能阻塞整个 Linux task 的 syscall；
- 运行期间 block asynchronous signals；
- 不支持 ptrace、single-step、breakpoint/watchpoint 与 live migration；
- SVE/SME/MTE 关闭；
- interrupt/EL1 entry 时 scheduler core quiesce，必须从同一 active context 返回 EL0。

这些是明确的实验边界，不是长期架构目标。

## 3. 系统组件与 ownership

| 组件 | 拥有的状态 | 不负责 |
|---|---|---|
| application core | 当前 `CPUARMState`、precise TB exit reason | 选择下一个 context |
| Remote Load Assist（RLA） | load classifier、PLT、commit/fault metadata | 执行用户 scheduler 代码 |
| Context Store | 非当前 coroutine 的完整 architectural context | OBMM route/transfer |
| scheduler core（SCC） | event queue、ready queue、policy、context state | 执行用户 coroutine payload |
| OBMM adapter | map/generation/token/coherence validation | 修改 EL0 PC/SP |
| provider | request/response 与 latency/failure model | 直接写 architectural register |
| guest control library/driver | context/map/fault registration、owner teardown | 每次 load 的 submit/await |

RLA 和 SCC 间的 `PENDING/COMPLETE/FAULT/EXIT` 是 core-internal event，不是 Arm
exception vector。应用看不到每次 load 的 token；token 只存在于 PLT/provider。

## 4. Control-plane ABI

### 4.1 为什么仍需要 ABI

普通 `LDR` 只解决数据访问表达，不会自动告诉硬件：哪些 stack 是 coroutine、当前
process 拥有哪些 context、哪些 GSVA map 可以 interception、失败由谁处理。因此
P2B 没有 per-load data-plane API，但不能没有 control plane。

### 4.2 EL0 library

```c
struct obmm_scc;

int obmm_scc_open(struct obmm_scc **out);
int obmm_scc_register_map(struct obmm_scc *scc,
                          int mapping_fd,
                          uint64_t mem_id,
                          void *gsva_base,
                          size_t length,
                          uint32_t flags,
                          uint64_t *policy_id);

int obmm_scc_context_create(struct obmm_scc *scc,
                            void (*entry)(void *),
                            void *arg,
                            size_t stack_bytes,
                            uint32_t flags,
                            uint64_t *context_id);
int obmm_scc_context_destroy(struct obmm_scc *scc, uint64_t context_id);

int obmm_scc_set_fault_context(struct obmm_scc *scc,
                               uint64_t fault_context_id);
int obmm_scc_run(struct obmm_scc *scc);
int obmm_scc_stop(struct obmm_scc *scc);
int obmm_scc_fault_resolve(struct obmm_scc *scc,
                           uint64_t fault_id,
                           uint32_t action);
```

`mapping_fd` 必须是创建 `gsva_base` 这段 VMA 的
`/dev/obmm_shmdev<mem_id>` fd，而不是 `/dev/obmm` 控制 fd。driver 用它同时校验
VMA file、`mem_id` 和当前 process 的 mapping ownership；传控制 fd 必须返回
`-EFAULT`，不能降级为只信任用户提供的地址。

这些调用只出现在 setup、teardown 和 failure recovery；正常 read data plane 仍是：

```c
uint64_t value = *(volatile uint64_t *)remote_ptr;
consume(value);
```

### 4.3 Context ID

```text
63                         32 31            16 15             0
+----------------------------+----------------+----------------+
| generation (32)            | home_core (16) | slot (16)      |
+----------------------------+----------------+----------------+
```

context create/destroy 后 slot reuse 必须增加 generation。PLT、ready queue 和
completion 都保存完整 context ID，禁止仅凭 slot 提交到新 context。

### 4.4 Driver/device contract

控制设备暂定 `/dev/linqu-scc0`，UAPI 放在
`guest-linux/kernel_ub/include/uapi/ub/obmm_scc.h`：

| ioctl | 作用 |
|---|---|
| `SCC_QUERY_CAPS` | ABI、context/PLT depth、支持的 load class |
| `SCC_REGISTER_MAP` | 交叉验证当前 process 的 OBMM mapping 并返回 policy ID/generation |
| `SCC_UNREGISTER_MAP` | 禁止新 interception；等待或 fault outstanding request |
| `SCC_CREATE_CONTEXT` | 校验 entry/stack，创建初始 context |
| `SCC_DESTROY_CONTEXT` | generation-safe teardown；有 pending 时返回 `-EBUSY` 或先 cancel |
| `SCC_SET_FAULT_CONTEXT` | 注册唯一 fault-service context |
| `SCC_START/STOP` | 绑定 home vCPU、owner address space、启停 interception |
| `SCC_GET_STATS` | 读取 event/switch/stall/fault counters |
| `SCC_RESOLVE_FAULT` | `ABORT_CONTEXT` 或 `RETRY_LOAD` |

driver 记录 process owner、home CPU 和 map ownership。device/RLA 还要校验当前 EL0
address-space identity；EL1 或其他 process/ASID 的 load 永不 interception。process
exit 时先 stop，再 cancel/fault outstanding load，最后释放 Context Store。

## 5. Hardware-visible state

### 5.1 Context Store entry

V1 每个 context 至少保存：

| 类别 | 字段 |
|---|---|
| identity | `context_id`、owner generation、home core、state |
| integer | `x0..x30`、`SP_EL0`、`PC` |
| PSTATE | NZCV、DAIF、BTYPE 及 QEMU `pstate_read()` 可见状态 |
| FP/SIMD | `Q0..Q31`、`FPCR`、`FPSR` |
| thread | `TPIDR_EL0`；V1 默认所有 context 初始化为相同值 |
| runtime | stack base/limit、exit trampoline、fault record pointer |
| load link | active PLT slot/generation，或 invalid |

系统寄存器、stage-1 page tables 和 EL1 state 属于 Linux task/vCPU，不按 coroutine
复制。exclusive monitor 在 switch 时清除；V1 不允许 exclusive 指令跨 switch。

QEMU 初版可以复制与上述字段对应的 `CPUARMState` subset；不能直接 `memcpy` 整个
`CPUARMState`，否则会复制 timer、MMU、EL1、device callback 等不属于 coroutine 的
状态。Context Store 的字节数、save/restore 字节数和模型周期必须进入 metrics。

### 5.2 Pending-load entry

每个 PLT entry 保存：

| 类别 | 字段 |
|---|---|
| identity | PLT slot/generation、provider token、context ID/generation |
| instruction | `fault_pc`、`next_pc=fault_pc+4`、`Rt`、instruction class |
| memory op | access size、zero/sign extend、endianness、MMU index、ordering class |
| address | effective VA、resolved map/policy ID、map generation、remote offset |
| result | 64-bit raw value、bytes done、status、fault code |
| timing | submit/complete cycle、deadline、scheduler event timestamps |
| state | `ALLOCATED/PENDING/COMPLETE/FAULTED/COMMITTED/RETIRED` |

PLT 必须在 provider submit 前完整写入。provider completion 只按完整 PLT token 查找，
不能保存 `CPUARMState *`、guest context pointer 或 `Rt` 的 host 地址。

### 5.3 Scheduler core event

```text
PENDING(context_id, plt_token)
COMPLETE(context_id, plt_token)
FAULT(context_id, plt_token, fault_code)
CONTEXT_EXIT(context_id)
OWNER_STOP(owner_generation)
```

event queue depth 固定 128；每个 event 带 generation。queue full 时 application core
不得丢 event：新 remote load 退化为 synchronous stall，completion event 保留在 PLT
并设置 scheduler wake level。

## 6. Precise load 时序

### 6.1 Pending path

1. Coroutine A 在 application core 执行白名单内普通 `LDR Xt, [Xn]`；
2. translation、permission、OBMM route/token/epoch/coherence 校验先完成；
3. local/cache hit 同步返回，load 正常退休，不触发 SCC；
4. remote miss 分配 PLT，形成 `PENDING(A, token)`；此时 `Rt` 未写、PC 未前进；
5. application core 退出当前 TB，保留 fault PC；
6. RLA 把 A architectural state 保存到 Context Store，A 变为 `WAIT_REMOTE`；
7. scheduler core 消费 event，round-robin 选择 READY 的 B；
8. context engine 恢复 B 到 application core，B 从自己的 PC 继续。

若无 READY context，application core idle；不是把 A 恢复后在原 load 内 host busy
poll。若 PLT/context/event capacity 不足，该次 load 使用同步 stall fallback，保持
普通 `LDR` 正确性并增加 `capacity_stall` counter。

### 6.2 Completion 与 retirement

1. provider 返回 `(plt_token, value | failure)`；
2. RLA 校验 PLT、context、map、owner generation；stale/late 只计数；
3. success：把 raw value 和 status 写入 PLT，发 `COMPLETE`，A 变为 `READY`；
4. scheduler core 选中 A 时，context engine 先恢复 A；
5. commit unit 按 access size/extend/endian 处理 value；若 `Rt != 31`，写入
   `env->xregs[Rt]`；
6. 设置 `env->pc = next_pc`，PLT 变为 `COMMITTED`，此刻原 load 才退休；
7. A 从 `LDR` 下一条指令继续，随后 PLT entry 才可按新 generation 复用。

禁止“恢复到原 PC 再重新发送 remote read”，否则会产生重复请求。也禁止 pending
时先把 PC 改成 next PC，因为 interrupt/debug/fault 会观察到未提交的 load 已退休。

### 6.3 Failure

普通 `LDR` 没有 errno 返回位，因此不能把 timeout/error 注入 0。V1 采用 fault-service
context：

1. 原 context 保持 `FAULTED`，`Rt` 和 PC 不提交；
2. RLA 写只读 `scc_fault_record`：context/PLT、PC、VA、map、status；
3. scheduler core 把注册的 fault-service context 置为 `READY`，入口 `x0` 指向
   fault record；
4. handler 只能选择 `ABORT_CONTEXT` 或 `RETRY_LOAD`；
5. 没有 fault-service context 或 handler 再失败时，driver 终止 SCC session，并向
   Linux task 报告 fail-stop error。

这是一条明确的 failure control path，不是每次 load 的 data-plane API，也不是直接
取到 EL0 的 Arm exception。

## 7. QEMU TCG 实现

### 7.1 为什么不能只改 `MemoryRegionOps.read()`

现有 MMIO `.read()` 只得到 address/size 并同步返回 `uint64_t`，不知道 AArch64
destination register、extend 方式和精确 instruction PC。若在这里返回 pending，
QEMU 无法知道 completion 应写 `X3` 还是 `W7`，也不能可靠地退休原指令。

因此 interception 必须从 AArch64 scalar-load translation path 携带：

```text
effective_address, Rt, MemOp, fault_pc, next_pc, mmu_index
```

### 7.2 Translation hook

`target/arm/tcg/translate-a64.c` 的 `do_gpr_ld_memidx()`/相关 scalar load lowering 是
候选入口。TB flags 增加 `SCC_ACTIVE`：

- SCC 未激活时生成代码与现状完全相同；
- 激活时，白名单 scalar load 先调用轻量 `scc_remote_load_try()`；
- helper 先做 owner/EL0/GSVA policy range gate；非目标地址返回 `NOT_REMOTE`，继续
  原 `qemu_ld`；
- local/cache hit 返回 `INLINE_VALUE`，translator 按正常路径写 `Rt`；
- remote miss 创建 PLT 后调用 `cpu_loop_exit_restore(cs, ra)`，不返回生成代码；
- translation/permission/token fault 走现有 precise exception 到 EL1，不转成 SCC
  pending。

概念接口：

```c
enum scc_load_try_result {
    SCC_NOT_REMOTE,
    SCC_INLINE_VALUE,
    SCC_PENDING,
    SCC_ARCH_FAULT,
};

enum scc_load_try_result
scc_remote_load_try(CPUARMState *env,
                    uint64_t va,
                    uint32_t rt,
                    MemOp memop,
                    uint64_t fault_pc,
                    uintptr_t retaddr,
                    uint64_t *inline_value);
```

实际 TCG helper 可以通过 `env` scratch/result fields 返回 status/value；上述签名只
冻结语义，不要求 C helper 直接返回两个值。

### 7.3 TB exit 与 context switch

`cpu_loop_exit_restore()` 是 QEMU 恢复 precise guest state 并跳出当前 TB 的机制。
它只负责让模拟 application core 停在 load 边界；退出后由 RLA/SCC 主循环：

```text
CPU exit reason == SCC_REMOTE_PENDING
  -> snapshot active context
  -> enqueue PENDING event
  -> model save + scheduler + restore cycles
  -> install selected context
  -> resume TCG execution
```

QEMU 不把 EL0 PC/SP 改成 scheduler trampoline。scheduler core 也不执行在当前
application core 的 `CPUARMState` 中。

### 7.4 Completion commit

provider callback 不能直接并发修改正在运行的 `CPUARMState`。它只更新 PLT 并排队
event；application core 在 QEMU CPU execution boundary 选择 context 时 commit。
该序列避免 host callback 与 vCPU 同时写 GPR/PC。

## 8. Context switch 与 Linux 交互

### 8.1 EL1 entry

remote pending 的 save/restore 只允许发生在 EL0。IRQ、syscall、page fault 等进入
EL1 时：

1. 锁定当前 active context；
2. SCC 不向 application core 安装另一个 context；
3. Linux 看到的 pt_regs 对应这个 active context；
4. `ERET` 回到同一 context 后才重新允许 scheduler event。

如果 Linux 把 owner task 调度出去，ASID/owner gate 关闭 interception；其他 process
不会继承 Context Store。V1 要求 home CPU affinity，禁止 migration。

### 8.2 Shared process semantics

各 coroutine 共享 address space、fd table、signal disposition 和默认 TLS。scheduler
core 只提供 execution context multiplexing，不提供 kernel thread 隔离。应用对共享
local memory 的 data race 仍由软件同步负责；一个 coroutine 的 blocking syscall 会
阻塞整个 Linux task，因此在 V1 workload 中禁止。

## 9. Ordering 与 correctness

- V1 只声明 relaxed normal load；`LDAR` 和 barrier 组合不支持；
- 对同一 context，remote load 未 commit 前没有后续 instruction 执行，因此保持该
  context 的依赖和程序顺序；
- 不把 context switch 当作跨 context 的 memory barrier；
- OBMM coherence acquire 在 pending request 被接受前完成；completion 时再次检查
  map generation/retire；
- completion value 只存在 PLT，直到 commit 才写 architectural register；
- X/W destination 的 zero extension、endianness 和 `Rt==31` 必须有 instruction-level
  golden tests；
- failed/stale/late completion 不修改 Context Store、GPR 或 PC。

## 10. Scheduler policy 与成本模型

V1 policy 为 generation-safe round robin：

```text
event PENDING(A): A -> WAIT_REMOTE; choose next READY
event COMPLETE(A): WAIT_REMOTE -> READY; append ready queue
event EXIT(A): RUNNING -> DONE; choose next READY
no READY: application core -> IDLE
capacity full: synchronous stall fallback
```

scenario YAML 增加 provider-neutral 模型，不使用临时环境变量：

```yaml
scheduler_core_model:
  enabled: true
  context_entries: 64
  pending_load_entries: 64
  event_queue_depth: 128
  save_cycles: 120
  schedule_cycles: 80
  restore_cycles: 120
  commit_cycles: 20
  clock_mhz: 2000
```

这些初始周期只用于 sweep，不是硬件结论。报告同时给出：

- modeled scheduler cycles；
- QEMU host wall time；
- guest-observed time；
- Context Store bytes moved；
- scheduler utilization、event queue high-water mark；
- `capacity_stall` 和 `no_ready_idle`。

P2B 是 demand-pending：默认到普通 `LDR` 真正 miss 才能切换。它不具备 P2A
pre-submit 的 schedule-ahead 优势；若未来加入 predictor/prefetch，必须作为独立 P3
变量报告，不能算入 P2B 基线。

## 11. 实现落点

| 顺序 | 文件/目录 | 实现内容 |
|---:|---|---|
| 1 | `guest-linux/kernel_ub/include/uapi/ub/obmm_scc.h` | caps、context/map/fault control ABI |
| 2 | `guest-linux/aarch64/driver/linqu_ub_drv.c` | SCC owner、ioctl、poll/fault、process teardown |
| 3 | `vendor/qemu_8.2.0_ub/include/hw/ub/ub_scc.h` | Context Store、PLT、event、cost model |
| 4 | `vendor/qemu_8.2.0_ub/hw/ub/ub_scc.c` | scheduler core state machine、ready/event queue |
| 5 | `vendor/qemu_8.2.0_ub/target/arm/cpu.h` | SCC-active/exit scratch state，不嵌入 guest pointer |
| 6 | `vendor/qemu_8.2.0_ub/target/arm/tcg/translate-a64.c` | scalar-load classification 与 helper lowering |
| 7 | `vendor/qemu_8.2.0_ub/target/arm/tcg/` 新 helper | remote try、precise TB exit、context commit |
| 8 | P1 `ub_obmm_remote.*`、`ub_ubc.c` | 共同 validation、result pool、provider completion；P2B sink 写 PLT/排 SCC event |
| 9 | `guest-linux/aarch64/libs/obmm_scc/` | control-plane library、stack/context bootstrap |
| 10 | `guest-linux/aarch64/apps/obmm_async_coroutine/` | `--mode scheduler-core`，与 P2A 共用 workload |
| 11 | `guest-linux/aarch64/scripts/build_initramfs.sh`、`initramfs/run_app` | build/package/dispatch |
| 12 | `guest-linux/aarch64/tests/`、QEMU qtest/TCG tests | ABI、instruction、state、script contracts |
| 13 | `scenarios/` | scheduler core capacity/cycle configuration |

P2B 与 P2A 共享 provider-neutral remote read backend 和同一 latency/failure injection，
但不共享 P2A 的 SQ/CQ data-plane ABI。

## 12. CLI 与可观测性

使用与 P2A 相同的唯一应用：

```text
obmm_async_coroutine \
  --mode scheduler-core \
  --coroutines 1|2|4|8|32|64 \
  --access-bytes 1|2|4|8 \
  --pattern sequential|random|dependent \
  --compute-us <N> \
  --iterations <N> \
  --deadline-us <N> \
  --seed <N> \
  --verify
```

结果行：

```text
OBMM_ASYNC_SUMMARY abi=1 mode=scheduler-core status=pass \
coroutines=8 completed=... failures=0 timeouts=0 stale=0 checksum=... \
pending_high=... ready_high=... switches=... capacity_stalls=... \
scc_util_milli=... scc_cycles=... context_bytes=... overlap_milli=...
```

QEMU trace events至少包括：

```text
scc_load_inline
scc_load_pending
scc_context_save
scc_context_restore
scc_load_complete
scc_load_commit
scc_load_fault
scc_stale_completion
scc_capacity_stall
```

每条 trace 带 owner/context/PLT generation 和 guest cycle；默认关闭，避免改变结果。

## 13. 测试设计

### 13.1 轻量/QEMU 单元与 TCG 测试

- Context ID/PLT token generation、wrap、stale completion；
- 64 context/64 PLT/128 event capacity 和 fallback；
- Context Store 精确保存/恢复 x0..x30、SP、PC、NZCV、Q0..Q31、FPCR/FPSR；
- `Rt=X0/X30/XZR`，1/2/4/8-byte zero extension，big/little-endian model；
- pending 时 `Rt` 不变、PC 保持 fault PC；commit 时只写目标寄存器且 PC+4；
- completion 不导致 instruction 重发；duplicate/late 不二次 commit；
- sync exception 在 pending 形成前仍由 EL1 路径接收；
- EL1 entry/return 期间不切 context；owner/ASID 不匹配时不 interception；
- fault-service context 的 abort/retry 与无 handler fail-stop；
- scenario/CLI/summary/build/run contract。

### 13.2 远端 QEMU 验证

- A 的普通 `LDR` pending 后，B 在同一 application vCPU 上推进；
- A 恢复后 PC、GPR、SIMD、stack canary、payload 与 sync baseline 一致；
- 无 READY context 时只 idle，无 host busy-spin；
- latency/jitter/reorder/timeout/retire/token denied/remote error；
- interrupt 在 save 前、save 后、completion 前、commit 前的 deterministic injection；
- 1/2/4/8/32/64 contexts 的 SCC event rate 和 capacity degradation；
- 固定 seed 下 event counts 和 checksum 可重复；
- guest 退出后无残留 QEMU process。

### 13.3 P2B 退出条件

1. success 路径普通 `LDR` 只发出一次请求，只提交一次 register value；
2. A 等待时 B 在同一 vCPU 推进；恢复 A 的完整 architectural state 正确；
3. failure 不注入 0，原 context 精确停在 fault PC，并由 fault-service/fail-stop 处理；
4. stale/late completion 无法命中新 context generation；
5. capacity full 有确定的同步 stall fallback，无 event/entry 丢失；
6. SCC save/schedule/restore/commit 成本被建模并与 host wall time分开；
7. 与 P2A 使用相同 workload、provider latency 和 checksum 形成可比较报告。

## 14. 实现顺序和停止条件

### B0：纯状态机

先实现 Context/PLT/event 的 host-side unit model，不接 TCG。若 generation、capacity、
fault 竞争不能通过测试，停止，不进入 CPU 修改。

### B1：单条 synthetic load

只支持 `LDR Xt`、单 application core、两个 context，完成 pending → B → commit A。
若 PC/Rt 精确性需要从 MMIO callback 猜 instruction，停止并回到 translation hook；
不得用 `return 0` 或 trampoline 绕过。

### B2：完整 V1 scalar whitelist

扩到 1/2/4/8-byte、XZR、interrupt quiesce、fault-service 和所有 capacity/failure case。

### B3：P2A/P2B 对比

固定 provider latency/cost，分别测：

- P2A demand await；
- P2A lookahead pre-submit；
- P2B demand pending；
- P2B demand pending + 不同 SCC cycle/capacity。

只有 B3 才能回答硬件透明性是否值得其 precise-state 和 Context Store 成本。

## 15. 尚未冻结的问题

- 目标硬件的 scheduler core 是固定状态机还是可编程 core；V1 QEMU 按独立有成本的
  状态机模拟；
- Context Store 最终落在 core-local SRAM、共享 SRAM 还是内存；
- 多 application core 共享 SCC 的公平性和 event routing；
- 是否值得支持 `LDAR`、SIMD/SVE 或 per-coroutine TLS；
- fault 是否最终映射为新架构 exception、Linux signal，还是继续使用 fault context；
- 是否加入硬件 prefetch/predictor 以弥补 demand-only 的 schedule-ahead 差距。

这些问题不会改变 V1 的核心正确性规则：未完成 load 不退休，结果只按 generation
提交到原 `Rt`，Context Store/SCC 拥有调度状态，失败绝不伪造成数值成功。
