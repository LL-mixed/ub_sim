# OBMM remote load：ESR_EL1 + Linux task + CQ/IRQ PoC

日期：2026-09-01
实现范围：`ub_sim`、QEMU TCG、Arm64 guest kernel、`linqu_ub_drv`、双节点 guest CLI

## 1. 结论

本 PoC 增加了一条 Linux 进程/线程级 remote-load 路径：EL0 线程执行普通
`LDR`；QEMU 判定目标地址属于已注册 remote region 后提交异步读取，并以
Arm64 data abort 把 remote-pending 原因送入 `ESR_EL1`；guest driver 在同步
异常快速路径中把当前 Linux task 挂到 waitqueue；远端完成事件写入 ABI v3
event ring，设备触发 IRQ；driver 从 ring 取出 completion 并唤醒 task；内核
从未修改的 `ELR_EL1` 返回，原 `LDR` 再执行一次，设备从 PLT replay entry
返回已完成的值。

这条路径保留现有 remote-region 注册控制面，并让 Linux scheduler 负责执行
上下文保存、task 选择和上下文恢复。EL0 coroutine scheduler、direct-EL0
upcall trampoline 和完整协程 context ABI 均不进入该模式的数据路径。

![ESR_EL1、Linux task、CQ/IRQ 与 replay 时序](2026-09-01-obmm-kernel-task-remote-load-poc.svg)

## 2. 目标与验收边界

PoC 回答四个问题：

1. 普通 EL0 `LDR` 遇到 remote pending 时，能否同步进入 EL1，并保留 faulting
   PC 供后续 replay。
2. 当前 Linux task 能否在内核快速路径中睡眠，使另一个 runnable task 获得
   CPU。
3. completion 能否通过 guest 可见 CQ/event ring 与 IRQ 唤醒正确 task。
4. 被唤醒 task 能否重新执行同一条 `LDR`，且只消费一次对应的 PLT 结果。

双节点验收要求 nodeA export 并写入每线程唯一值；nodeB 至少创建两个
pthread；每个 pthread 从 nodeA remote mapping 执行普通 8-byte scalar load；
日志需要同时证明 fault、pending、task sleep、completion、IRQ drain、task
wakeup、replay consume 和最终值校验。

## 3. ABI 扩展

### 3.1 ESR_EL1 编码

| 字段 | PoC 取值 | 含义 |
|---|---:|---|
| `ESR_EL1.EC` | `0x24`，Data Abort from lower EL | EL0 load 同步进入 EL1 |
| `ESR_EL1.ISS.DFSC` | `0x3a` | UB remote load pending，implementation-defined |
| `FAR_EL1` | remote effective VA | driver 与 pending event 的地址匹配键 |
| `ELR_EL1` | faulting `LDR` PC | 返回 EL0 后 replay 同一条指令 |

`DFSC=0x3a` 在 Arm 架构当前 fault status 表中属于 implementation-defined
空间。QEMU 与该 guest kernel 共同定义其语义；普通 Arm64 主机不会自行产生
这个原因。

### 3.2 OBMM async-load UAPI

现有 ABI version 保持为 3，并通过 capability 与 start flag 做向后兼容扩展：

| 名称 | 值 | 用途 |
|---|---:|---|
| `OBMM_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY` | `1ULL << 12` | 设备与 driver 支持 Linux-task replay |
| `OBMM_ASYNC_LOAD_START_KERNEL_TASK` | `1UL << 1` | session 选择 Linux-task 路径 |
| `OBMM_ASYNC_LOAD_START_REPLAY_RETIRE` | `1UL << 0` | 强制 completion 使用 replay retirement |
| `GET_KERNEL_TASK_STATS` | ioctl `0x10` | 读取 fault/event/sleep/wakeup/error 计数 |

kernel-task flag 必须与 replay flag 同时设置；`upcall_entry` 必须为 0。该约束
保证同一 session 只有一种 delivery/retirement 语义。

### 3.3 CQ 与 IRQ

PoC 复用 ABI v3 DMA-coherent event ring 作为 completion queue：

- producer header 维护 `producer_sequence`；
- driver consumer page 维护 `consumer_sequence`；
- PENDING、COMPLETE、FAULT 沿用 128-byte event schema；
- kernel-task session 使用 `event.reserved[0]` 携带 `TPIDR_EL0` thread cookie，
  `reserved[1..2]` 必须为 0；该 cookie 与 fault PC、VA 一起完成 task/event 匹配；
- 新增 async-load IRQ status/ack 寄存器 `0x1a0/0x1a8`；
- COMPLETE/FAULT 发布后置位 completion bit，并通过 UBC 的 dedicated async-load
  IRQ output 通知 `linqu_ub_drv`；
- CQ 尚有待处理 completion 时 IRQ 保持高电平；driver drain CQ 并写 ACK 后，
  QEMU 重新读取 consumer sequence；已发布 event 尚未全部消费时继续保持 IRQ，
  全部消费后撤销 IRQ。这条规则封闭了 drain 与 ACK 之间新 completion 到达时的
  lost-interrupt 窗口。

独立 sim-decoder driver 当前没有形成单独的 guest 模块。PoC 复用已经绑定
sim-decoder/UBC MMIO 与 IRQ 的 `linqu_ub_drv`，由它完成 ring drain 和 task
wakeup。后续若拆分设备所有权，CQ 消费契约与 fault hook 可以迁入独立驱动，
UAPI 无需随之改变。

## 4. 分组件实现

| 组件 | 修改位置 | 实现职责 |
|---|---|---|
| QEMU async-load device | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load_device.c` | capability、kernel-task session、线程 context cookie、event publish、IRQ status/ack、level-held completion IRQ |
| QEMU UBC/virt wiring | `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`、`vendor/qemu_8.2.0_ub/hw/arm/virt.c` | dedicated async-load sysbus IRQ output、guest DT SPI 资源、IRQ level 控制 |
| QEMU Arm64 TCG helper | `vendor/qemu_8.2.0_ub/target/arm/tcg/helper-a64.c` | 识别 kernel-task mode，构造 Data Abort syndrome，设置 FAR/FSR，送往 EL1；replay 命中后返回 load value |
| Arm64 ESR 定义 | `guest-linux/kernel_ub/arch/arm64/include/asm/esr.h` | 定义 `ESR_ELx_FSC_REMOTE_LOAD=0x3a` |
| Arm64 fault 分派 | `guest-linux/kernel_ub/arch/arm64/mm/fault.c` | fault table index 58、可注册 handler、module lifetime 保护 |
| kernel hook API | `guest-linux/kernel_ub/include/linux/arm64_remote_load.h` | driver 注册/注销同步 fault handler |
| async-load UAPI | `guest-linux/kernel_ub/include/uapi/ub/obmm_async_load.h` | capability、start flag、stats 与 ioctl |
| guest driver | `guest-linux/aarch64/driver/linqu_ub_drv.c` | fault/event 匹配、waitqueue sleep、IRQ drain、task wakeup、统计、错误闭环 |
| guest CLI | `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c` | `--kernel-task-replay --threads N`、双节点 producer/consumer、pthread load 与值校验 |
| E2E runner | `guest-linux/aarch64/scripts/run_ub_obmm_eval.sh` | cmdline 转换、日志门禁、artifact 指纹、精确 QEMU 清理 |

## 5. 数据路径

### 5.1 pending 路径

1. nodeB pthread 在已注册 GSVA mapping 上执行普通 `LDR`。
2. QEMU TCG helper 完成正常 stage-1 access probe，再由 async-load device 解析
   remote mapping。
3. device 分配 PLT entry，提交 remote read，向 event ring 发布 PENDING。
4. helper 产生 lower-EL Data Abort，`DFSC=0x3a`，`FAR=VA`，异常目标为 EL1。
5. Arm64 fault table 调用已注册的 driver handler。
6. driver drain PENDING，以 `TPIDR_EL0 cookie + fault_pc + effective_va` 找到
   PLT token，记录当前 PID，然后调用 `wait_event_killable_timeout()`。
7. Linux scheduler 保存当前 task 上下文，并选择另一个 runnable task。

### 5.2 completion 与 replay 路径

1. remote backend 完成读取，QEMU 更新 PLT 为 replay-ready。
2. QEMU 发布 COMPLETE/FAULT event，置位 async-load IRQ status，并拉高 dedicated
   async-load IRQ。
3. `linqu_ub_drv` IRQ handler drain CQ，验证 owner generation、sequence、context、
   token、PC、flags 与 status，再唤醒 token 对应 waitqueue；handler 写 ACK 后，
   QEMU 在 status 清空时撤销 IRQ。
4. 原 task 被 Linux scheduler 重新调度，fault handler 返回 0。
5. 异常返回保持 `ELR_EL1`，原 `LDR` 再执行。
6. QEMU 以 context identity、fault PC、VA、width 和目标寄存器匹配 PLT replay
   entry，把远端值作为本次 load 结果并原子消费 entry。

## 6. 上下文身份与并发规则

QEMU PoC 读取 `TPIDR_EL0` 作为线程 cookie。glibc pthread 为每个线程配置独立
thread pointer，因此同一进程内的 pthread 能映射到不同 async-load logical
context。QEMU 把 cookie 放入 kernel-task event 的 `reserved[0]`；同步异常入口
读取当前 `TPIDR_EL0`，driver 用 cookie、fault PC 和 VA 做精确匹配。该值是 guest
线程标识，UAPI 不暴露 host 地址。

当前并发约束：

- 一个 async-load device 同时只允许一个 active session；
- session 绑定一个 TTBR0 owner 和一个 home vCPU；
- consumer 进程及其 pthread 继承单 CPU affinity；
- logical context 数量上限为设备 `context_entries`，当前为 64；
- 每个 PLT slot 同时服务一个 pending load；
- driver 通过 owner generation 拒绝跨 session stale event。
- session 内每个活跃 pthread 必须保持唯一且稳定的 `TPIDR_EL0`；重复 cookie
  会共享 logical context，并触发 fail-closed 风险。

硬件化时更稳健的接口应提供显式 task/context registration，并由计算单元提供
不可伪造的 context tag。`TPIDR_EL0` 适合验证 pthread 并发语义，尚未达到
跨 runtime、跨语言 ABI 的产品要求。

## 7. 错误语义

| 条件 | PoC 行为 |
|---|---|
| 无 active kernel-task session | fault handler 返回错误，Arm64 fault core 向 EL0 发送 `SIGBUS/BUS_OBJERR` |
| owner/TGID 不匹配 | 拒绝 fault |
| ring schema、sequence、token 或 context 不匹配 | `protocol_errors++`，fail closed |
| remote completion status 非 SUCCESS | 唤醒 task，fault handler 返回 `-EIO`，EL0 收到 `SIGBUS` |
| deadline 超时 | `timeouts++`，返回 `-ETIMEDOUT` |
| task 收到 signal | `interrupted_waits++`，返回 wait error |
| replay key 不匹配 | device 计入 `replay_mismatch` 并 fail stop |

## 8. CLI 与双节点验证

runner 的核心参数形态：

```text
--mode async-load
--kernel-task-replay
--threads 2
--deadline-us 1000000
--producer-index 0
--expected-outcome success
--verify
```

验收日志包括：

- nodeA：`OBMM_ASYNC_LOAD_WRITE`、`OBMM_ASYNC_LOAD_EXPORT`；
- nodeB user：`OBMM_ASYNC_LOAD_KERNEL_THREAD`、
  `OBMM_ASYNC_LOAD_KERNEL_TASK_SUMMARY`；
- nodeB kernel：`remote-load pending/block/completion/wake`；
- runner：`OBMM_ASYNC_LOAD_KERNEL_TASK_EVIDENCE`；
- artifact：scenario/model/QEMU/kernel/initramfs SHA-256；
- cleanup：本次 pid file 对应的 QEMU 均已退出。

验收器还会逐线程比较 nodeA 写入值、nodeB expected 值与 nodeB actual 值，
要求 fault/pending/completion/wakeup/replay 均为精确一次，要求 direct EL0
upcall 计数为 0，并要求至少两个不同 PID 进入 remote-load waitqueue。

### 8.1 n4-910c 双节点实跑结果

最终验收 run 为 `kernel-task-replay-poc-20260901-r8`。远端证据目录：

```text
n4-910c:/home/ll/ub_sim_kernel_task_20260901/
  out/kernel-task-replay-poc/kernel-task-replay-poc-20260901-r8.log
  guest-linux/aarch64/logs/
    kernel-task-replay-poc-20260901-r8_headless8/nodeA_guest.log
    kernel-task-replay-poc-20260901-r8_headless8/nodeB_guest.log
    kernel-task-replay-poc-20260901-r8_headless8/nodeA_qemu.log
    kernel-task-replay-poc-20260901-r8_headless8/nodeB_qemu.log
```

| 验收项 | R8 结果 |
|---|---|
| nodeA export/write | `export_mem_id=1`，offset 4096/8192 写入两个不同 64-bit 值 |
| nodeB Linux tasks | PID 178、179，各执行一次普通 scalar `LDR`；fault PC 均为 `0x403190`，logical context 分别为 `0x100000000`、`0x100000001` |
| fault 与阻塞 | `faults=2`、`pending=2`、`sleeps=2` |
| CQ/IRQ 与唤醒 | `completions=2`、`wakeups=2`，两个 waitqueue 均由 dedicated IRQ 路径唤醒 |
| replay 与值 | `replay_consumed=2`；两个 actual 值逐项等于 nodeA 写入值 |
| 隔离性 | `direct_el0_upcalls=0`、`replay_mismatch=0` |
| 错误闭环 | `protocol_errors=0`、`timeouts=0`、`interrupted_waits=0` |
| 单次 load 观测延迟 | thread 0：15,431,328 ns；thread 1：17,632,864 ns |
| 进程清理 | `qemu_destroyed=1`，验收结束后无残留 `qemu-system-aarch64` |

R8 artifact 指纹：

| artifact | SHA-256 |
|---|---|
| QEMU | `4cdd27463b13b57743d80ade012ff076e930cd93558f1b147e43407b7fcf6606` |
| guest kernel Image | `b7f0634fa2d31950b560846230ea3234193faecb601b305e09aabc9c11d8e703` |
| initramfs | `4113265db347ca4e1ab1b276cba7df48c2bacec6ee6cb2592c395f8943e788c2` |
| `linqu_ub_drv.ko` | `95742d4fd076ff4222840b856ece314b57b5e8010b779654f2bb31c590fbccf0` |
| scenario | `dde4d793e72725d1f0c2effc1b777fa07968a48c8f02187118bf113092671009` |
| remote-memory model | `e8d7d2e291a9612e1d8b95f78ddee56069d22bc3b4b0256cc3fb6b8cec271f04` |

R5 首次证明 dedicated IRQ 能进入 driver，同时暴露 waitqueue wake mask
不匹配：fault path 使用 `wait_event_killable_timeout()`，completion path 当时使用
`wake_up_interruptible()`，task 只能在 `deadline + HZ` 到期后重新检查完成条件，
观测延迟约 2 秒。R6 将每个 PLT slot 的单 waiter 唤醒改为 `wake_up()`；修复后
completion 到 wake 的时间恢复到毫秒量级，且全部语义门禁继续通过。R5 只作为
缺陷定位证据。R7 在此基础上增加 thread cookie 精确匹配和 ACK 时 consumer
sequence 复核。R8 进一步要求 COMPLETE/FAULT 的 cookie、fault PC 和 VA 与
PENDING 完全一致，并将 kernel-task capability 检查与 EL0 scheduler capability
分离；最终验收以 R8 为依据。

### 8.2 回归验证

| 验证 | 命令/入口 | 结果 |
|---|---|---|
| QEMU async-load model | `build_qemu_binary.sh --with-obmm-tests` 后运行 `test-ub-async-load` | 9/9 通过，包含 PENDING/COMPLETE cookie 贯穿测试 |
| QEMU OBMM backend | `test-ub-obmm-remote-model`、`test-ub-obmm-remote` | 13/13 通过 |
| guest/UAPI 契约 | `python3 -m unittest discover guest-linux/aarch64/tests` | 359/359 通过 |
| Rust workspace | `cargo test --workspace -- --test-threads=1` | workspace tests 与 doctests 全部通过 |
| SVG | `xmllint` 与 Inkscape PNG render | XML 有效，1600×1050 render 成功 |

Rust 测试并发执行时，仓库既有的 fused-SIMT launch self-check 偶发命中临时
可执行文件 `ETXTBSY`。失败日志保存在
`out/kernel-task-replay-poc/cargo-r8-full.log`；同一代码状态下的串行全量测试
保存在 `out/kernel-task-replay-poc/cargo-r8-full-serial.log`，结果通过。

## 9. 当前限制与后续方向

| 限制 | 影响 | 后续方向 |
|---|---|---|
| QEMU TCG 专用 remote DFSC | 无法直接在未扩展的 Arm CPU 上运行 | 在真实计算单元定义可架构化的 pending-load exception/notification contract |
| `TPIDR_EL0` 线程 cookie | runtime 依赖较强，可由 EL0 写入 | 增加受保护 context registration/tag |
| 单 home vCPU | 尚未覆盖多核 task migration | 增加 per-vCPU owner、migration handshake 与 per-CPU CQ/IRQ |
| event ring 兼作 CQ | 功能闭环成立，队列隔离和 IRQ moderation 尚简化 | 增加 phase/owner、batch drain、coalescing 与 backpressure |
| fault hook 仅允许一个 provider | 多设备并存时需要分派 | 以 FAR range/device owner 建立 handler registry |
| driver 使用 `dev_info` 记录每个事件 | 会显著干扰性能 | 验收后切换 tracepoint 与采样日志 |
| 当前只验证 scalar load | vector、atomic、exclusive、store 未覆盖 | 明确指令白名单并增加拒绝/回退测试 |

本 PoC 的性能定位是语义验证。同步异常、Linux scheduler、IRQ 与日志引入的
固定成本明显高于 EL0 coroutine direct-upcall 路径；它换取现成的进程/线程
编程模型、内核 task 生命周期、signal 与调度能力。后续性能评估应在关闭逐事件
日志后，按 remote latency、runnable task 数、CPU 频率、IRQ batching 和负载
依赖性建立策略选择表。
