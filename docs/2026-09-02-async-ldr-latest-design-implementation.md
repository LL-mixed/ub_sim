# 透明异步 LDR 最新方案设计实现

日期：2026-09-02

状态：与主仓库 HEAD（`8e2e274`）、QEMU HEAD（`a23329e24e`）和 guest kernel
HEAD（`e60874b807`）一致。control ABI 4 + event ABI 3 已统一为 replay-only；
Normal NC 与 Normal Cacheable 均已完成 two-node arm64 guest 功能验收。

## 1. 结论

当前最新方案把"普通 `LDR` 透明访问远端 OBMM 内存"拆成两条 replay-only 路径，按
PTE memory type 分流：

| memory type | outstanding state | remote completion 的数据归宿 | 原 `LDR` 退休方式 |
|---|---|---|---|
| Normal Cacheable | 普通 cache/MSHR/fill state | 完整 64-byte line 进入普通 fill/coherence pipeline | 从原 PC replay，命中已填充的 cache line |
| Normal Non-cacheable | requester UBC 的 NC PLT | scalar result 与 status 留在 NC PLT | 从原 PC replay，one-shot token 精确消费 NC PLT |
| Device | 透明异步首版拒绝 | 同步访问或显式异步接口 | 由所选接口定义 |

两条路径共享同一条主链路：UBC 接受远端请求后立即给 core 一个 **void response**
（无数据），core 产生精确的 `REMOTE_PENDING` Data Abort（或 EL0 模式下 direct
upcall），软件把当前执行上下文挂起并让出 CPU；远端数据到达后由 UBC 发布
completion CQE 并拉起 IRQ；软件恢复原上下文，标准 `ERET` 回到原 PC，原 `LDR`
重放并退休。**replay 不发起第二次远端读**。

![Normal Cacheable 与 Normal NC completion replay](plans/2026-09-02-remote-completion-replay-paths.svg)

chip side changeset 被压缩到三处：

1. core 新增一个 implementation-defined Data Abort DFSC（`0x3a`），复用现有精确异常
   入口、`ESR/FAR/ELR` 编码和标准 `ERET`；
2. requester UBC 新增一个 one-shot replay-arm latch（仅 NC 路径使用），以
   `{owner/context, token, replay_pc}` 约束下一次 eligible `LDR`，消费后自动清除；
3. remote-specific outstanding state（NC PLT、transport-fill 关联、CQ/IRQ）收敛到
   requester UBC；Cacheable miss 继续使用普通 cache/MSHR/fill。core、cache 和 MSHR
   都不保存 task/coroutine/scheduler 状态。

调度完全交给现有软件：Linux scheduler（kernel-task 模式）或 guest EL0 coroutine
runtime（EL0 模式）。此前版本中的私有 `HLT #0x5343/44/45` assist、QEMU 侧 832-byte
context install、PATCH retirement 均已删除。

## 2. 演进脉络

| 时间 | 提交 | 变化 |
|---|---|---|
| 2026-08-12 | QEMU `b3bb87b497` 等，主仓库 `08ff7e2` | ABI v2：自定义 direct EL0 upcall（PC 重定向）+ `HLT #0x5343` resume / `#0x5344` wait / `#0x5345` scheduler-enter + guest EL0 coroutine scheduler；PATCH 或 REPLAY 两种 retirement |
| 2026-08-22 | QEMU `049a0648f7`，主仓库 `e925c07` | ABI v3：kernel-free 事件环（DMA coherent ring），事件投递零 ioctl；仍保留三条私有 HLT |
| 2026-09-01 | 主仓库 `43a1945`，QEMU `6ac3fb70d3` | kernel-task replay PoC：PENDING 改用真正的 Data Abort 投递，task 睡 Linux waitqueue，completion 走 CQ/IRQ 唤醒，调度主体换成 Linux scheduler |
| 2026-09-01 | 主仓库 `9f9f30a` | trace-off 调度器对比：EL0 coroutine vs Linux task，给出按延迟选型的初始策略 |
| 2026-09-02 | 主仓库 `3c85e90`，QEMU `3e0c6d6b4b` | Normal NC replay-only：删除三条私有 HLT 与 PATCH retirement；resume 改私有 SVC fast path + 标准 `ERET`，idle wait 改标准 `WFE`；QEMU model 正式命名 NC PLT |
| 2026-09-02 | 主仓库 `8e2e274`，QEMU `a23329e24e` | Normal Cacheable fill replay：PENDING 先于 remote submit 发布，64-byte line fill 先于 CQE 完成，全程绕过 NC PLT |

演进方向单一：把 PoC 早期"自定义指令 + 自定义 context ABI"的核心变更，逐步替换成
arm64 现有异常/等待/返回机制，新增硬件状态持续向 UBC 一侧收敛。

## 3. 最小化 chip side changeset

### 3.1 复用的 arm64 core 现有机制

最新方案刻意只让 core 做它本来就会做的事：

- **精确 Data Abort 异常入口**：remote `LDR` 不退休、以 faulting PC 进入 EL1，与
  普通 page fault 同一条路径；`do_mem_abort` 的 fault 分派表只需注册一个新 FSC
  handler（`arch/arm64/mm/fault.c` 的 `do_remote_load_fault`）。
- **标准 syndrome 编码**：`ESR_EL1` 的 ISV/SAS/SRT/SF 字段如实记录被拦截 `LDR`
  的 size、目标寄存器和位宽，`FAR_EL1` 记录 effective VA，`ELR_EL1` 记录 faulting
  `LDR` 的 PC。唯一新增的是 DFSC 取值 `0x3a`（`ESR_ELx_FSC_REMOTE_LOAD`，
  implementation-defined abort 空间）。
- **标准 `ERET`**：恢复时 fault handler 直接返回，`ELR_EL1` 从不被修改，异常返回
  路径自然从原 PC 重放 `LDR`。契约测试显式断言 fault handler 函数体内不存在
  `regs->pc =`。
- **标准 SVC**：EL0 coroutine 的 context resume 与 scheduler-enter 走私有 SVC
  immediate（`#0x5343`/`#0x5345`），`do_el0_svc` 对非零 immediate 先调注册的
  driver callback，落选再回落普通 syscall。SVC handler 把目标 GPR/SP/PC/NZCV 装进
  当前 `pt_regs`，经标准异常返回恢复协程。
- **标准 `WFE`**：EL0 scheduler 无 READY 协程时执行 `WFE` 睡眠，completion IRQ
  唤醒；`SEVL; WFE` + 最后一次 CQ drain + 第二次 `WFE` 的协议关闭 lost-wakeup
  窗口。
- **TLB/PTE memory type**：Cacheable 与 NC 的分流不引入新属性，直接读取 stage-1
  MAIR 编码的 `pte_attrs`（outer nibble 非 0 且非 `0x44`/`0x40` 判为 Normal
  Cacheable），import 侧的 cache 属性由既有 OBMM import 路径
  （`OBMM_IMPORT_CACHE_NC`/`OBMM_IMPORT_CACHE_CC`）决定。
- **普通 cache/MSHR/fill**：Cacheable remote line 与 local line 使用完全相同的
  entry 格式和 fill contract；replay `LDR` 通过普通 tag/data lookup 命中。

### 3.2 core 侧不维护的东西

下列状态曾出现在 PoC 早期版本或直觉方案中，当前全部禁止进入 core：

- per-outstanding-load table（PLT 在 UBC，不在 core）；
- coroutine Context Store、ready queue、round-robin 等 scheduler 状态；
- 832-byte context image 的保存/恢复逻辑（QEMU 私有 HLT resume 已删除）；
- `Rt`/`PC` patch（PATCH retirement 已删除，CQE 不再携带 scalar value）。

NC 路径的 one-shot replay-arm latch 位于 requester UBC：resume 前由 driver 通过
device command arm，绑定 owner/context 与 replay PC，下一次 eligible `LDR` 到达 UBC
时精确消费并自动清除。CPU core 不维护该 latch。Cacheable 路径连这个 UBC latch
也不用——resume 前没有任何 device command。

### 3.3 硬件组件职责边界

| 组件 | 负责 | 明确禁止 |
|---|---|---|
| CPU core | 识别受支持的 EL0 scalar `LDR`；产生精确 `REMOTE_PENDING`；保存标准异常 PC/VA/PSTATE/syndrome；replay 时重新发出原 load | per-outstanding-load table、replay token latch、context/scheduler state |
| cache + MSHR | 普通 miss 分配、merge waiter、coherence、fill；replay 命中 | 保存 NC result、replay-ready 状态、task/coroutine ID、PC/SP/`Rt`、saved context、CQ ownership、scheduler state |
| requester UBC | void response；NC PLT；one-shot replay-arm latch；transport transaction 到 fill transaction 的关联；fill unit 移交 cache hierarchy；CQE + IRQ | 为 Cacheable load 分配 NC PLT；保留 replay-ready scalar result（Cacheable 路径） |
| Linux driver / EL0 runtime | `replay_token`/`wait_key` 到 task/coroutine 的会合；context 保存与状态机；选择下一个执行上下文；resume 前 arm token（NC） | 把 CQE value 写入 saved `Rt`；`saved_pc += 4` |

### 3.4 被删除的私有机制清单

- `HLT #0x5343` context resume、`HLT #0x5344` idle wait、`HLT #0x5345`
  scheduler-enter（QEMU `3e0c6d6b4b` 删除全部 helper 与 `trans_HLT` 拦截分支，
  `trans_HLT` 回归只剩 semihosting）；
- QEMU 侧 832-byte `UbAsyncLoadEl0Context` 的保存/原子安装机制；
- PATCH retirement（`3e0c6d6b4b` 起 session START 强制 `REPLAY_RETIRE`，COMPLETE
  event 的 `value` 恒为 0）；
- EL0 协程的 `completion_mode` 选项（guest 库 replay-only，`replay_retire` 字段移除）。

保留的自定义语义只有两处：direct EL0 upcall 的 PC 重定向（EL0 模式的 PENDING 事件
投递，`env->pc = upcall_entry` 后 `cpu_loop_exit_noexc`），以及 DFSC `0x3a`。前者
只存在于 EL0 coroutine 模式，kernel-task 模式完全使用标准 Data Abort。

“kernel-free”在当前 revision 中只描述 EL0 对 event descriptor 的读取、校验、ack
和 coroutine 状态转换。idle wakeup 通过 completion IRQ，context resume 通过 SVC
fast path，两者都会进入 EL1。详细 ABI 边界见
[ABI v3 EL0 event ring 与 SVC/WFE](plans/async-load-abi-v3-kernel-free-event-ring.md)。

## 4. 统一主链路

两条 memory-type 路径共享的骨架如下，差异只在"结果放哪、replay 怎么取值"。

```text
LDR 命中 remote map
  → UBC 接受请求，立即 void response（无数据返回）
  → PENDING event 先于 remote request submission 发布
  → core: kernel-task 模式 raise 精确 Data Abort(DFSC=0x3a)
          EL0 模式 direct upcall(PC → upcall_entry)
  → 软件会合 {wait_key/token, context cookie, PC, VA}，挂起当前上下文
  → Linux scheduler 或 EL0 round-robin 运行其他 task/coroutine
  → 远端响应到达 requester UBC
      NC:        scalar result → NC PLT，entry → REPLAY_READY
      Cacheable: 64B fill unit → 普通 cache fill 可见
  → COMPLETE CQE 可见 → producer sequence 可见
      running EL0 coroutine: 下一精确 boundary direct upcall
      all-blocked / Linux task: completion IRQ
  → 软件 drain CQ，按 wait_key/token 找到原上下文并恢复
      NC:        arm one-shot replay token（device command）
      Cacheable: 无 device command
  → 标准 ERET 回到原 PC → 原 LDR 重放
      NC:        精确消费 NC PLT result，token 自动清除
      Cacheable: 普通 cache lookup 命中已填充 line
```

关键 ordering 不变式：

- PENDING 路径固定为 `future reserved → PENDING event visible → remote request
  submitted → Data Abort raised`。先占 result slot 再发请求，capacity 不足走
  backpressure，不允许先发请求再找槽位。
- NC completion 路径固定为 `PLT.value/status visible → PLT.state = REPLAY_READY →
  CQE visible → producer sequence visible → IRQ/event`。
- Cacheable completion 路径固定为 `remote payload complete → cache fill visible →
  COMPLETE CQE visible → producer sequence visible → IRQ asserted → driver drain CQ →
  wake_up(task)`。driver 在观察到 COMPLETE 之前不得唤醒 task。

## 5. Normal NC：UBC NC PLT 与 token-directed replay

NC 属性 import 的远端内存不经过 cache hierarchy，每条 outstanding dynamic load 需要
一个独立的 result 归宿，这就是 requester UBC 里的 NC PLT。

### 5.1 NC PLT entry 与状态机

最小 entry（silicon contract，见 `docs/plans/2026-09-02-normal-nc-replay-plt-svc-eret-design.md`）：

```c
struct nc_plt_entry {
    uint32_t slot_generation;
    uint16_t owner_id;
    uint8_t state;
    uint8_t access_bytes;
    uint32_t session_generation;
    uint32_t mapping_handle;
    uint32_t mapping_generation;
    uint32_t status;
    uint64_t remote_offset;
    uint64_t value;
};
```

entry 不保存 coroutine context、task、SP、`Rt` 或 ready queue；replay token 负责选择
dynamic load，address/size/mapping generation 负责防止错误消费。QEMU 功能模型中的
对应物是 `UbAsyncLoadNcPltEntry`（`hw/ub/ub_async_load.c`），额外携带完整
`UbAsyncLoadDesc` 供 replay 时逐字段比对。

状态机主路径：`FREE → PENDING → COMPLETE → REPLAY_READY → REPLAY_ARMED →
CONSUMED → FREE`。terminal fault 发布 FAULT CQE 后立即回收 entry；PLT 满属于正常
backpressure（`SYNC_STALL` 退化为同步 load），不进 fail-stop。

### 5.2 Replay token

token 沿用 slot/generation 思路：`replay_token = owner/session identity + slot +
slot generation`，打包为 `generation<<32 | owner_id<<16 | slot`（NC PLT owner_id=2，
与 Cacheable wait_key 的 owner_id=3 命名空间隔离）。系统不变式：

- live token 唯一，terminal response 到达前与 replay 消费前均禁止 slot reuse；
- session reset 递增 generation；
- replay lookup 验证 owner、mapping、address、size，任何不匹配 fail-stop；
- CQE 只携带 readiness/status/token，**不携带 scalar value**——CQ consumer 不读取
  也不 patch value，数据只能由 replay `LDR` 从 NC PLT 取出。

### 5.3 One-shot arm 与精确消费

resume 前由软件执行 device command（QEMU 功能模型为 MMIO
`REPLAY_CONTEXT_ID/REPLAY_TOKEN/REPLAY_PC` + `REPLAY_COMMAND=1`）：校验 entry 属于
该 context、状态为 `REPLAY_READY`、`fault_pc` 精确相等后，entry 转 `REPLAY_ARMED`。
恢复原上下文后，`ERET` 回到原 PC，`LDR` 重放，core（QEMU 中为
`ub_async_load_replay_consume_token`）要求 entry 为 `REPLAY_ARMED` 且 desc 全字段
一致（fault_pc、va、map_id/generations、remote_offset、rt、bytes、mmu_index、
sign、endian、memory type），满足则返回 payload、回收 entry、token 自动清除；任何
不一致 `REPLAY_MISMATCH` + fail-stop。重复消费、stale generation、错 context、错
fault_pc、重复 arm 均被拒绝，由单测锁定。

### 5.4 两种软件调度主体

同一 NC PLT/token 语义下有两种 delivery/调度 owner，session 二选一：

| | kernel-task replay | EL0 coroutine |
|---|---|---|
| PENDING delivery | 精确 Data Abort（DFSC `0x3a`） | direct EL0 upcall（PC 重定向） |
| 挂起方式 | task 睡 `wait_event_killable[_timeout]` | 协程置 `WAIT_REMOTE`，EL0 保存 832B context |
| 调度主体 | Linux scheduler | EL0 round-robin |
| idle wait | Linux 调度器自然让出 | `SEVL; WFE` + drain + `WFE`，completion IRQ 唤醒 |
| resume | fault handler arm token 后直接返回，标准 `ERET` | `SVC #0x5343`（x0=context、x1=replay_token），kernel 装入 `pt_regs`，标准 `ERET` |
| fault 语义 | 错误返回 → `SIGBUS/BUS_OBJERR` | 协程置 `FAULTED` |

2026-09-01 trace-off 对比（2 节点 QEMU/TCG，4 contexts × 64 ops，latency 1µs–10ms）
给出的初始策略：≤10µs 选 EL0（吞吐高 30–40%，P99 低约 3×）；~1ms 批处理 makespan
可选 Linux task（+8.3%）但 P99 仍以 EL0 为优；10ms 两者打平，按编程模型选择。
TCG 时间不可换算 silicon，该结论只用于选型方向。

该矩阵还早于 2026-09-02 的 HLT 删除、SVC/WFE 收敛与 Cacheable 实现。它只描述
历史 artifact，不能充当当前 revision 的正式性能阈值；恢复 P3 后需要重新测量。

## 6. Normal Cacheable：void response + remote completion 普通 fill

### 6.1 为什么绕开 NC PLT

Cacheable 访问的结果归宿是普通 cache hierarchy：远端响应到达后，完整 fill unit 由
普通 MSHR/fill 路径安装，replay `LDR` 用普通 tag/data lookup 命中。MSHR 本来就保存
line address、fill ownership、merge waiter 与 coherence state，local 与 remote line
没有格式差别，因此不需要任何额外的 per-load result table。`wait_key` 只承担 software
wait/wakeup correlation（取 ordinary miss transaction identity，QEMU 功能模型中为
backend future 的 generation/slot），生命周期止于 CQE 被 driver 消费并唤醒 task。

这条路径全程禁止分配 NC PLT。验收硬门禁为 `nc_plt_allocations=0`、
`nc_plt_pending=0`、`nc_replay_consumed=0`，QEMU 单测
（`cacheable-bypass-nc-plt`）在模型层锁定：Cacheable 的 pending/complete API 不触碰
`nc_plt[]`，`nc_plt_allocations` 只在 NC 分配函数中递增。

### 6.2 链路与职责

1. EL0 `LDR` 产生普通 cache miss，requester UBC 接受远端请求；
2. UBC 立即 void response 并发布 `REMOTE_PENDING`，core 以精确 lower-EL Data
   Abort 进入 EL1（`ESR_EL1`/`FAR_EL1`/`ELR_EL1` 携带 fault reason、VA、原 PC）；
3. Linux fast path 把当前 task 绑定到 `wait_key` 并进入 waitqueue，Linux scheduler
   运行其他 runnable task；
4. UBC 收到远端响应后把完整 fill unit 交给普通 cache fill 路径；
5. fill 可观察完成后发布 completion CQE，再触发 IRQ；
6. driver drain CQ、按 `wait_key` 唤醒原 task；Cacheable 成功路径不 arm 任何
   token，fault handler 直接返回；
7. 标准异常返回 `ERET`，原 `LDR` 从原 PC replay，命中已填充的 line。

### 6.3 QEMU 功能模型映射

QEMU TCG 没有 timing-accurate 的 cache/MSHR 层级，功能验证采用如下映射
（silicon contract 不受影响）：

| silicon contract | QEMU 功能模型 |
|---|---|
| PTE memory type | 从 arm64 TLB full entry 的 `pte_attrs` 识别 Normal Cacheable |
| ordinary miss identity | async backend future 的 generation/slot，仅保留 transport 生命周期 |
| immediate void response | remote submit 前发布 PENDING event，随后 raise Data Abort |
| ordinary fill | 复用 sim-decoder page-cache，新增 64-bit line-valid bitmap |
| fill hit | replay helper 查询同一 page-cache，line valid 后放行 translated load |
| completion CQ/IRQ | 复用 ABI v3 event ring、producer sequence 与 async-load IRQ |

line-valid bitmap 位于 `SimDecPageCacheEntry.valid_line_mask`：4 KiB 页对应 64 条
64-byte line，整页填充置 `UINT64_MAX`，远端 64-byte line fill 只置对应位段；CPU
window 读只有在目标 line 全部 valid 时才被服务，未填 line 继续走远端读。该
surrogate 不进入 silicon contract，也不构成 hardware remote line fill table；真实
cache timing、MSHR pressure 与 coherence 性能仍需 timing model 或 RTL 验证。

## 7. 两条路径对照

| 项目 | Normal Cacheable | Normal Non-cacheable |
|---|---|---|
| outstanding memory state | 普通 cache/MSHR/fill state | requester UBC NC PLT |
| `wait_key` 来源 | 普通 miss transaction identity（owner_id=3） | NC PLT slot/generation token（owner_id=2） |
| remote response data | 完整 fill unit 进入普通 cache | scalar value 进入 NC PLT |
| completion CQE | readiness、status、`wait_key` | readiness、status、NC replay token |
| resume 前的 device command | 无 | arm one-shot NC replay token |
| 原 `LDR` replay | 普通 cache hit | 精确消费 NC PLT result |
| NC PLT allocation | 恒为 0 | 每条 outstanding dynamic load 一项 |
| core 新增状态 | 无（DFSC 除外） | 无（DFSC 除外）；token latch 位于 requester UBC |

## 8. 指令子集与 fail-stop

首版透明异步 `LDR` 只支持 EL0 unsigned scalar load（1/2/4/8 bytes，unsigned-offset
或 register-offset，无 writeback，`Rt=XZR` 允许丢弃结果）；拒绝 signed load、pair、
SIMD/FP、exclusive/atomic/acquire、pre/post-index writeback 和 Device memory。

进入 owner/session fail-stop 的情况：replay token 命中错误 owner、generation 不匹配、
replay PC/address/size/mapping generation 不匹配、duplicate replay consume、event/CQ
sequence corruption、arm 尚未 `REPLAY_READY` 的 token、REPLAY_READY 后地址不再命中
remote map。PLT capacity 不足是 backpressure 而非 fail-stop，必须提供 per-owner
quota、per-PE reservation、high-water 与 stall counter。

## 9. 验证状态

2026-09-02 在 `n4-910c1` 完成的验收（详细证据见第 10 节两份当日设计文档）：

| campaign | 路径 | 关键结果 |
|---|---|---|
| `normal-nc-replay-el0-20260902-r5` | NC + EL0 coroutine | 2 协程、2 direct upcall、2 replay consume、0 mismatch、CQE 不带 value，pass |
| `normal-nc-replay-kernel-task-20260902-r2` | NC + Linux task | 2 线程、2 次 FSC `0x3a` fault、2 sleep/wakeup、2 replay consume，pass |
| `cacheable-esr-cq-20260902-r4` | Cacheable + Linux task | 2 个 64-byte fill、2 次 ERET replay hit、`nc_plt_allocations=0`、PENDING-before-submit 顺序门禁，pass |
| `nc-future-reg-20260902-r2` | NC 回归 | 2 NC PLT allocation、2 replay consume、Cacheable 计数全 0，pass |

回归：QEMU async-load unit 11/11、remote backend/model 6/6 + 7/7、async-load contract
16/16、guest Python contract 390/390、Rust workspace pass、`cargo fmt --check` pass。

尚未形成实跑结论的范围：timing-accurate cache/MSHR/coherence hierarchy；Cacheable
的 EL0-coroutine delivery；Device memory 透明异步；fault-at-original-`LDR` replay 与
完整 `CANCEL_REQUESTED/ORPHANED` 生命周期；timeout/cancel/late completion 的
two-node failure matrix；trace-off 正式性能 campaign。禁止从当前功能日志推导这些
范围的性能或 RTL 结论。

## 10. 提交与文件索引

关键提交：

- QEMU：`b3bb87b497`（split-phase + EL0 upcall）、`aa9039e507`（replay
  retirement）、`049a0648f7`（ABI v3 kernel-free ring）、`6ac3fb70d3`（kernel-task
  exception delivery）、`3e0c6d6b4b`（NC replay-only，删 HLT）、`a23329e24e`
  （Cacheable fill replay）；
- 主仓库：`08ff7e2`（EL0 coroutine 路径）、`e925c07`（ABI v3）、`43a1945`
  （kernel-task PoC）、`9f9f30a`（调度器对比）、`3c85e90`（NC replay-only）、
  `8e2e274`（Cacheable 验收）。

当日设计文档：

- `docs/plans/2026-09-02-normal-nc-replay-plt-svc-eret-design.md`：NC PLT entry/CQE
  结构、token 契约、SVC/WFE/ERET resume ABI、状态机与 fail-stop；
- `docs/plans/2026-09-02-normal-cacheable-void-response-esr-cq-validation-design.md`：
  Cacheable 链路、硬件组件职责、QEMU surrogate 映射、验收证据；
- `docs/plans/2026-09-01-obmm-kernel-task-remote-load-poc.md`、
  `docs/plans/2026-09-01-obmm-el0-coroutine-vs-kernel-task-trace-off.md`：kernel-task
  PoC 与调度器对比。

关键代码位置：

| 层 | 位置 |
|---|---|
| QEMU NC PLT/事件模型 | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load.c` |
| QEMU 设备/future/ring/IRQ | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load_device.c` |
| QEMU page-cache/fill surrogate | `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` |
| QEMU LDR 拦截与 Data Abort | `vendor/qemu_8.2.0_ub/target/arm/tcg/{translate-a64.c,helper-a64.c}` |
| kernel FSC/fault 分派/SVC hook | `guest-linux/kernel_ub/arch/arm64/{include/asm/esr.h,mm/fault.c,kernel/syscall.c}` |
| Linux driver | `guest-linux/aarch64/driver/linqu_ub_drv.c` |
| EL0 coroutine runtime | `guest-linux/aarch64/libs/obmm_coroutine_scheduler/` |
| UAPI | `guest-linux/kernel_ub/include/uapi/ub/obmm_async_load.h`（control ABI 4、event ABI 3） |
| workload/CLI | `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c` |
| acceptance runner | `guest-linux/aarch64/scripts/run_ub_obmm_eval.sh` |
