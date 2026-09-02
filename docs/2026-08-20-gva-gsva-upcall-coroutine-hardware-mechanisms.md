# GVA、GSVA 与 Async Load PoC 的硬件机制拆解

初始日期：2026-08-20

现行修订：2026-09-02

代码基线：ub_sim `8e2e274001306a040eb62bb2e65197205e600518`

QEMU 基线：`a23329e24ef4f4b37e024e72f19b2ac7fcd0b590`

guest kernel 基线：`e60874b8072d7dddce033377a2173200b8d8c855`

## 1. 结论

当前 PoC 覆盖三组相关机制：

1. **GVA** 把进程 VA、S3 route 和 UB address 连接起来；
2. **GSVA** 在多节点建立同一对象的共享 VA 语义，并增加 key、generation、
   lifecycle 与 coherence 约束；
3. **Async load** 在远端 `LDR` 延迟较长时发布 PENDING，调度其他 execution
   context，completion 后恢复原 `LDR` replay。

Async load 的目标硬件状态按 memory type 分开：

- Normal Cacheable 使用普通 cache/MSHR/fill state；远端 line 与本地 line 采用同一
  cache-line contract；
- Normal Non-cacheable 使用 requester UBC 内的 NC Pending Load Table（NC PLT）
  保留 one-shot scalar result；
- Device memory 暂不进入透明 async `LDR` 首版。

计算单元无需通用 pending-load table。Cacheable 路径也无需 remote line fill table。
NC PLT 只覆盖 Normal NC，且不保存 task/coroutine context 或 scheduler state。

现行 CPU/OS contract 使用 implementation-defined Data Abort、`ESR_EL1`、CQ/IRQ、
`SVC` fast path、`WFE` 与标准 `ERET`。三个旧私有 HLT assist 已删除。EL0 coroutine
mode 仍保留 direct EL0 upcall；Linux-task mode 使用同步异常进入 EL1。

![GVA、GSVA 与 async load 的硬件/软件边界](2026-08-20-gva-gsva-upcall-coroutine-hardware-components.svg)

## 2. 术语与分层

| 术语 | 含义 | 归属 |
|---|---|---|
| GVA | Global Virtual Address；进程地址经过 MMU/S3 route 形成 UB 可路由访问 | architecture + control plane |
| GSVA | 多节点共享同一 VA/object semantic identity 的 GVA 模式 | architecture + OS/control plane |
| GVA manager | 建立、发布、撤销 mapping 和 route metadata 的软件服务 | 软件，不是硬件块 |
| UBC | 计算单元连接 UB 的协议、route、request/completion 前端 | 硬件目标；QEMU device 仿真 |
| MSHR | Cache miss outstanding state；跟踪 line address、fill owner、merge waiter | 普通 cache hierarchy |
| NC PLT | Normal NC async load 的 Pending Load Table | requester UBC |
| CQE | completion queue entry；发布 readiness、status 和 correlation key | device/driver ABI |
| `wait_key` | software wait/wakeup correlation identity | software queue + CQE |
| coroutine scheduler | guest EL0 runtime 的 context、ready queue 与 policy | guest EL0 软件 |
| Linux scheduler | task/thread context switch 与 runnable policy | guest kernel 软件 |
| direct EL0 upcall | 精确 EL0 boundary 把 PC 转到注册 trampoline 的自定义 core 行为 | 实验性 CPU extension |
| replay | 保留 faulting PC，resume 后重新执行原 `LDR` | CPU + memory hierarchy/UBC |

本文把 silicon contract、guest software 和 `simulation-only` 功能模型分开描述。

## 3. 总体组件边界

数据路径可以压缩为：

```text
EL0 LDR
  -> CPU MMU/LSU
  -> local cache lookup
  -> requester UBC
  -> S3 route / GSVA semantic checks
  -> UB Link
  -> home UBC / memory controller
  -> remote response
  -> requester cache fill or NC PLT
  -> CQ/IRQ or direct EL0 event
  -> software scheduler wakeup
  -> original LDR replay
```

控制路径为：

```text
GVA manager + OS + libobmm
  -> object/map identity
  -> PTE attributes + S3 route + UBC mapping
  -> owner/session/generation
  -> revoke/drain/unmap
```

## 4. 按硬件组件拆解

### 4.1 CPU 前端、译码、LSU 与 retirement

#### 目标职责

- 识别受支持的 EL0 scalar `LDR`；
- 保持标准 translation、permission、alignment 和 memory-type fault 优先级；
- 对 accepted remote pending 产生精确 architectural boundary；
- 保留 faulting PC；
- resume 后从原 PC replay；
- 保持 load ordering 与 memory type 规则。

CPU 前端不保存 remote result，也不保存 task/coroutine ready state。

#### 当前 QEMU

`target/arm/tcg/helper-a64.c` 在正常 translated load 前查询 async-load device。
helper 读取 TLB full entry 的 PTE attributes，区分 Normal Cacheable 与 Normal NC。

Linux-task mode 产生 lower-EL Data Abort：

| register | contract |
|---|---|
| `ESR_EL1.EC` | `0x24` |
| `ESR_EL1.ISS.DFSC` | `0x3a`，implementation-defined `REMOTE_PENDING` |
| `FAR_EL1` | effective VA |
| `ELR_EL1` | faulting `LDR` PC |

EL0 coroutine mode 在 PENDING 或运行态 completion 时直接改 PC 到注册 trampoline，
随后退出当前 TB。该行为属于实验性 direct-upcall delivery。

### 4.2 Arm MMU、TLB 与 memory attributes

MMU 负责：

- EL0 VA translation 和 permission；
- ASID/VMID/TTBR owner identity；
- memory type 与 shareability；
- GVA/GSVA S3 translation 所需的地址和 context；
- revoke 后的 TLB invalidation。

memory attributes 决定 completion result 的落点：

| PTE type | result retention | replay path |
|---|---|---|
| Normal Cacheable | 普通 line fill | cache lookup |
| Normal NC | NC PLT scalar entry | replay-token lookup |
| Device | 透明 async load 拒绝 | 同步或显式 async API |

### 4.3 普通 cache、MSHR 与 fill pipeline

Normal Cacheable remote miss 使用普通 cache hierarchy：

- MSHR 保存 line address、fill ownership、merge waiter 和 coherence state；
- remote response 以完整 fill unit 返回；
- fill 对 replay load 可见后才能发布 completion CQE；
- replay `LDR` 进行普通 tag/data lookup。

MSHR 禁止保存 task/coroutine ID、PC、SP、`Rt`、saved context、CQ owner 或 scheduler
state。local 与 remote line 使用相同 entry format。

当前 QEMU TCG 缺少 timing-accurate cache/MSHR。PoC 复用 sim-decoder page cache，
在现有 4-KiB entry 内增加 64-bit line-valid bitmap，表达 64 个 64-byte line。该
bitmap 只做功能可见性验证。

### 4.4 Requester UBC

Requester UBC 负责：

- 接收 remote miss 或 NC access；
- route/ownership/generation 检查；
- 建立 transport transaction correlation；
- 在 remote submit 前发布 PENDING；
- Cacheable response 交给普通 fill pipeline；
- Normal NC response 写入 NC PLT；
- fill/result 可见后发布 CQE 与 IRQ；
- 处理 timeout、stale、duplicate、cancel 和 capacity。

QEMU 中的 generic future 保存 transport 生命周期。它不承担 architectural replay
result retention，也不等同于 NC PLT。

### 4.5 Normal NC PLT

NC PLT 仅服务 Normal NC dynamic load。最小 entry 保存：

- slot/generation；
- owner/session；
- mapping handle/generation；
- access size 与 remote offset；
- terminal status；
- scalar value；
- state。

目标状态机为：

```text
FREE -> RESERVED -> PENDING_REMOTE -> REPLAY_READY
     -> REPLAY_ARMED -> CONSUMED -> FREE
```

entry 不保存 coroutine context、Linux task、SP、PC、`Rt` 或 ready queue。软件用
replay token 关联 waiter；CPU/UBC replay lookup 再校验 owner、mapping、address、size
和 generation。

### 4.6 CQ、IRQ 与 `wait_key`

completion CQE 只发布 readiness、status 和 correlation identity：

- Cacheable CQE 携带 `wait_key`；
- Normal NC CQE 携带 replay token；
- architectural value 留在 cache 或 NC PLT。

发布顺序固定为：

```text
result/fill visible
  -> terminal state visible
  -> CQE descriptor visible
  -> producer sequence visible
  -> IRQ/direct-upcall notification
```

`wait_key` 生命周期：

1. load accepted 时，由普通 miss identity 或 NC token 生成；
2. PENDING event 将 key 交给 software；
3. driver/runtime 建立 `wait_key -> task/coroutine` waiter；
4. CQE 返回同一个 key；
5. consumer 校验 owner/session/map generation；
6. waiter 进入 ready/runnable；
7. CQE 被消费且 waiter 已唤醒后，software correlation entry 回收；
8. Normal NC 的 NC PLT entry 继续保留到原 `LDR` 消费 token。

参与组件为 requester UBC、CQ producer、guest driver 或 EL0 runtime waiter table、
software scheduler。`wait_key` 本身不进入 context image。

### 4.7 Exception、SVC 与 ERET

Linux-task mode：

1. PENDING 以 FSC `0x3a` 进入 EL1；
2. driver 将当前 task 放入 waitqueue；
3. Linux scheduler 运行另一 runnable task；
4. completion IRQ drain CQ 并唤醒目标 task；
5. Normal NC 路径 arm replay token；
6. fault handler 返回；
7. 标准 exception exit 执行 `ERET`；
8. 原 `LDR` replay。

EL0 coroutine mode 的 context resume 使用 `SVC #0x5343` fast path。coroutine
scheduler 选择目标 context，driver 校验 image/session，按需 arm NC token，并把
GPR/SP/PC/PSTATE 安装到 `pt_regs`。标准 exception exit 返回目标 context。

`SVC #0x5345` 标记 coroutine 正常退出后进入 scheduler state。旧私有 HLT immediate
不再被 QEMU 拦截。

### 4.8 Direct EL0 upcall delivery

该组件只服务 EL0 coroutine mode：

- PENDING descriptor publish 后，PC 转到注册 trampoline；
- 运行态 completion 在下一个精确 EL0 boundary 交付；
- trampoline 在使用 scratch register 前保存 GPR/SIMD/FP/TLS；
- event descriptor 由 EL0 从 ABI v3 ring 读取；
- `upcall_active` 防止嵌套 PC redirection。

direct upcall 不携带 ready queue policy，不安装目标 context，也不读取 remote value。
目标硬件若保留该 mode，需要定义调试、signal、single-step、RAS 与嵌套异常交互。

### 4.9 S3 route engine、`mp_table` 与 GVA

GVA 数据面需要以下硬件可执行状态：

- `{VMID, ASID, VA/UBA range}` route match；
- destination CNA/EID/port；
- permission、memory type 与 generation；
- route miss/fault；
- revoke 与 invalidation。

GVA manager 是软件控制面。它通过 kernel/driver 接口编程 route 与 mapping；硬件只消费
已经安装的表项。

### 4.10 GSVA semantic engine

GSVA 在 GVA route 之上增加：

- object/key identity；
- segment token 与 generation；
- home/owner；
- publish/acquire/revoke state；
- mapping epoch；
- stale rejection；
- coherence policy。

Async-load request 若要达到完整 GSVA contract，需要把 mapping handle/generation 与
GSVA key/token/epoch 统一到同一 request identity。当前分段 PoC 尚未完成该联合验收。

### 4.11 Coherence directory

Normal Cacheable GSVA/OBMM line 进入普通 coherence domain。directory 负责 sharer、
owner、invalidate、writeback 与 generation。Async-load completion 必须在 line fill 和
coherence permission 可观察后发布 CQE。

Normal NC 不分配 cache line，不进入 ordinary line fill。ordering 与 visibility 仍按
Normal NC memory type contract 处理。

### 4.12 UB Link、home UBC 与 memory controller

UB Link endpoint 负责 packet、flow control、retry、CRC、duplicate suppression 与
terminal status。home UBC/UMMU 重新验证 destination mapping 与 permission，memory
controller 访问 DRAM/HBM，并把 response 与 request identity 一起返回。

requester 在超时后仍可能收到 late response，因此 generation 与 cancel/orphan lifecycle
必须防止旧 response 污染新 transaction。

### 4.13 Device-SVA master

NPU、SSD 或其他 UB-attached master 可以复用 GVA/GSVA translation 和 object semantic
identity。CPU async `LDR` 的 EL0 upcall、task waitqueue 与 SVC context resume不适用于
设备 command queue；device 采用自己的 SQ/CQ、interrupt 和 retry contract。

### 4.14 Timer、RAS 与 telemetry

目标硬件至少需要：

- outstanding/capacity/high-water；
- timeout、cancel、late、duplicate、stale；
- Cacheable fill 与 NC PLT 分流 counter；
- CQ overflow 与 IRQ coalescing；
- owner/session fail-stop；
- route/coherence/replay mismatch syndrome。

QEMU latency timer、jitter/drop/error model、virtual clock 和逐事件 trace 属于
`simulation-only`。目标硬件只需要架构化 timeout/RAS/telemetry contract。

## 5. 软件组件职责

### 5.1 GVA manager、OS 与 libobmm

- 创建和解析 object/mapping identity；
- 建立 export/import；
- 安装 PTE、S3 route、GSVA token 与 UBC map；
- 选择 Normal Cacheable 或 Normal NC；
- 管理 owner/session/generation；
- revoke、drain、unmap。

### 5.2 EL0 coroutine scheduler

- 保存完整 coroutine context；
- 消费 ABI v3 event ring；
- 维护 waiter、ready queue 和 policy；
- 选择下一个 coroutine；
- 通过 SVC 恢复 context；
- all-blocked 时执行 WFE wait protocol。

### 5.3 Linux driver 与 scheduler

- 配置和隔离 device/session；
- 处理 FSC `0x3a`；
- 维护 task waitqueue；
- drain CQ/ack IRQ；
- 唤醒 task；
- 通过标准 exception exit 完成 ERET replay。

## 6. 关键状态对象归属

| state | owner | lifetime | 禁止混入的内容 |
|---|---|---|---|
| PTE/TLB entry | MMU/OS | map 到 invalidate | scheduler policy |
| route entry | S3 route engine | install 到 revoke | coroutine context |
| MSHR/fill | cache hierarchy | miss 到 fill/abort | task ID、PC/SP、CQ owner |
| transport future | requester UBC | submit 到 terminal response | architectural result retention |
| NC PLT entry | requester UBC | NC submit 到 replay consume/fault retire | full context、ready queue |
| NC replay-arm latch | requester UBC | resume command 到下一次 eligible `LDR` consume/mismatch | full context、scheduler policy |
| CQE | device/CQ | publish 到 consumer ack | Cacheable line、NC scalar retirement value |
| `wait_key -> waiter` | driver或 EL0 runtime | PENDING consume 到 wake/cleanup | cache/PLT payload |
| coroutine context | guest EL0 runtime | coroutine create 到 destroy | QEMU scheduling policy |
| Linux task context | guest kernel | task lifetime | device replay result |
| GSVA token/epoch | semantic engine/control plane | acquire 到 revoke | application register state |

## 7. 两条 completion replay 路径

### 7.1 Normal Cacheable

```text
LDR miss
  -> ordinary MSHR/fill transaction
  -> PENDING before remote submit
  -> software schedules another context
  -> remote 64-byte line
  -> ordinary fill visible
  -> CQE + IRQ/upcall
  -> waiter ready
  -> original PC
  -> ordinary cache-hit replay
```

当前 two-node Linux-task acceptance 已通过，NC PLT allocation 为 0。

### 7.2 Normal NC

```text
LDR accepted
  -> NC PLT allocation
  -> PENDING before remote submit
  -> software schedules another context
  -> remote scalar result
  -> NC PLT REPLAY_READY
  -> CQE + IRQ/upcall
  -> waiter ready + arm replay token
  -> original PC
  -> one-shot NC PLT consume
```

当前 two-node EL0-coroutine 与 Linux-task acceptance 均已通过。

## 8. 已验证范围

| capability | evidence | result |
|---|---|---|
| GVA/GSVA 分段 PoC | 既有 2/4-node 报告与 route/coherence logs | pass，详见对应历史报告 |
| ABI v3 EL0 ring | sequence、descriptor、hot-path event handling | pass |
| Normal NC EL0 coroutine | `normal-nc-replay-el0-20260902-r5` | 2 value、2 replay、0 mismatch，pass |
| Normal NC Linux task | `normal-nc-replay-kernel-task-20260902-r2` | 2 value、2 wake、2 replay，pass |
| Normal Cacheable Linux task | `cacheable-esr-cq-20260902-r4` | 2 fill、2 ERET replay、NC PLT=0，pass |
| Normal NC regression after shared future | `nc-future-reg-20260902-r2` | 2 NC PLT allocation/consume，Cacheable counter=0，pass |

Normal Cacheable 的 QEMU page-cache/line-valid 实跑证明功能 ordering、value 与
path separation。它不证明真实 cache latency、MSHR pressure、coherence throughput
或 silicon area/power。

## 9. 当前缺口

1. Cacheable EL0-coroutine two-node acceptance；
2. GSVA key/token/epoch/coherence 与 async load 的同一 revision 联合验收；
3. multi-core、task migration、per-CPU CQ/IRQ；
4. timeout/cancel/late/duplicate real-guest matrix；
5. NC PLT capacity/quota/orphan/RAS 的 silicon design；
6. timing-accurate cache/MSHR/fill 与 coherence model；
7. SVC/WFE 当前 revision 的 trace-off 性能矩阵；
8. ObjectRef、mem_service、OBMM mapping 和真实模型消费点的无旁路 E2E。

## 10. 对用户与系统的影响

| mode | 用户接口 | 系统代价 | 适用方向 |
|---|---|---|---|
| sync | 普通阻塞 load | 最小软件复杂度，core 等待远端 | 延迟短、并发低 |
| submit/await | 显式 future/CQ | 需要 API/编译器/runtime 协同 | 可提前发起、批量并行 |
| async load + EL0 coroutine | 普通 `LDR` + scheduler 注册 | direct upcall、event ring、SVC/WFE | 指针访问透明、用户态协程已有 |
| async load + Linux task | 普通 `LDR` + task mode | Data Abort、waitqueue、IRQ | 复用进程/线程编程模型 |

运行时可以共存这些机制，并在 session 或 mapping 建立时按 memory type、remote latency、
runnable contexts、tail SLO 与 workload compute 选择策略。

## 11. 代码与文档追踪

| 主题 | 入口 |
|---|---|
| 当前 async-load 总览 | [实现总结](plans/async-load-implementation-summary.md) |
| ABI v3 EL0 ring | [event ring 与 SVC/WFE](plans/async-load-abi-v3-kernel-free-event-ring.md) |
| Normal NC | [NC PLT、SVC 与 ERET](plans/2026-09-02-normal-nc-replay-plt-svc-eret-design.md) |
| Normal Cacheable | [void response、ESR、CQ/IRQ 与 replay](plans/2026-09-02-normal-cacheable-void-response-esr-cq-validation-design.md) |
| Linux task | [ESR_EL1 + task + CQ/IRQ PoC](plans/2026-09-01-obmm-kernel-task-remote-load-poc.md) |
| GVA/GSVA | `sim_gva_simulation_design.md`、`sim_gsva_shared_virtual_address_design.md` |

## 12. 最终判断

当前实现已经证明：Normal Cacheable 与 Normal NC 都能在 remote pending 时让软件
scheduler 运行其他 execution context，并在 completion 后从原 PC replay；两条路径的
result-retention state 已清楚分离。

硬件最小化方向已经明确：Cacheable 复用普通 cache/MSHR/fill，Normal NC 增加边界
清楚的 NC PLT，CPU 复用 Data Abort、SVC、WFE 与 ERET。剩余工作集中在真实 timing、
多核、failure lifecycle、GSVA 联合语义和 Lingqu workload 集成。
