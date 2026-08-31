# Async-load ABI v3 kernel-free event ring 设计

## 1. 目标与验收边界

ABI v3 将 async-load 正常运行时的 event data plane 完整放到 guest EL0：

- QEMU/计算单元发布 `PENDING`、`COMPLETE`、`FAULT` event；
- EL0 coroutine scheduler 直接读取 event payload 并确认消费进度；
- 全部 coroutine 阻塞时，EL0 使用 atomic wait assist 挂起当前 vCPU；
- completion 到达后，QEMU/计算单元唤醒同一个 vCPU；
- coroutine 正常退出后，EL0 使用 scheduler-enter assist 进入调度状态；
- patch/replay completion policy 保持为 EL0 runtime policy。

kernel 负责 setup、ownership、DMA memory allocation/mapping、session lifecycle、
map registration、统计读取与异常回收。`START` 到 `STOP` 之间的 event hot path
禁止调用 `GET_EVENT`、`SCHEDULER_ENTER`、`poll`、futex、signal 或其他等待 syscall。

ABI v3 acceptance 需要同时满足：

1. 每个 event 的 descriptor 在 producer sequence 发布前完整可见；
2. EL0 使用 acquire load 观察 producer sequence；
3. EL0 使用 release store 发布 consumer sequence；
4. QEMU 校验 owner generation、sequence 单调性和 ring 容量；
5. direct upcall 改写 EL0 PC 后立即退出当前 TB；
6. all-blocked wait 的空队列检查与 vCPU halt 构成一个原子操作；
7. completion 发布后能够唤醒 wait assist 挂起的 vCPU；
8. patch/replay 2-node E2E 的 hot-path ioctl count 均为零。

## 2. 总体数据流与映射布局

![ABI v3 event ring 与 kernel-free wait/wakeup](async-load-abi-v3-event-ring-flow.svg)

driver 为每个 async-load file allocation 分配两个 DMA-coherent mapping。

### 2.1 Producer/event ring mapping

该 mapping 对 EL0 只读，由 QEMU/计算单元写入。开头是 64-byte producer header，
包含 ABI version、depth、slot bytes、owner generation 与 producer sequence；其后
紧跟 `depth` 个 128-byte event slot。

producer sequence 从 0 开始。sequence 为 `N` 的 event 使用：

```text
slot = (N - 1) % depth
```

QEMU 先 DMA 写入 slot，执行 publish barrier，最后 DMA 写入
`producer_sequence=N`。

### 2.2 Consumer mapping

该 mapping 对 owner EL0 可读写，QEMU/计算单元只读。固定字段为 ABI version、owner
generation、consumer sequence、wait count 与 scheduler-enter count，其余空间保留。

EL0 完成 event validation 和 state transition 后，以 release store 更新
`consumer_sequence`。QEMU 在 publish、wait、resume、scheduler-enter 时读取并校验该值。

以下情况进入 fail-stop：

- consumer sequence 回退；
- consumer sequence 超过 producer sequence；
- owner generation 不匹配；
- producer 与 consumer 的距离超过 depth；
- descriptor sequence 与目标 sequence 不一致；
- ring DMA read/write 失败。

## 3. Event descriptor

每个 slot 固定为 128 bytes。字段包含：

```c
struct obmm_async_load_event_v3 {
    __u64 sequence;
    __u64 owner_generation;
    __u64 context_id;
    __u64 plt_token;
    __u64 interrupted_pc;
    __u64 fault_pc;
    __u64 effective_va;
    __u64 value;
    __u64 map_id;
    __u64 map_generation;
    __u64 model_phase_generation;
    __u32 kind;
    __u32 status;
    __u16 rt;
    __u16 access_bytes;
    __u32 flags;
    __u64 reserved[3];
};
```

`interrupted_pc` 只描述 direct upcall 打断的 EL0 instruction boundary。
EL0 scheduler 已经处于 all-blocked wait 状态时，该字段为 0。

## 4. CPU assist

ABI v3 使用三个 simulator-private AArch64 assist：

| immediate | 名称 | 作用 |
| --- | --- | --- |
| `HLT #0x5343` | resume | 原子安装 EL0 选择的完整 coroutine context |
| `HLT #0x5344` | wait | 原子检查 pending event；空队列时挂起 vCPU |
| `HLT #0x5345` | scheduler-enter | coroutine 正常退出后原子屏蔽 direct upcall 并进入 EL0 scheduler state |

这些 HLT immediate 只在 active async-load EL0 session 中具有自定义语义。其他
session、其他 EL 或其他 immediate 沿用 QEMU 原行为。

### 4.1 Wait race closure

wait helper 在 QEMU device lock/BQL 保护下执行：

1. 校验 session、owner CPU、`upcall_active` 和 consumer sequence；
2. event model 已有 event 时，将一个 event 发布到 ring 并退出 TB；
3. event model 为空时设置 `scheduler_waiting=true`、`cpu->halted=1`，随后退出 CPU loop；
4. completion callback 在同一保护域中产生 event；
5. callback 观察到 `scheduler_waiting` 后发布 event、清除 halted 并 kick vCPU。

这个顺序关闭“EL0 观察到空队列以后、vCPU 真正 halt 以前 completion 已到达”的
lost-wakeup 窗口。

## 5. Direct upcall 时序

remote `LDR` 产生 PENDING event 时：

1. async-load model 分配 PLT entry 并生成 PENDING；
2. QEMU 将 PENDING descriptor 写入 event ring；
3. QEMU release-publish producer sequence；
4. QEMU 设置 `upcall_active`；
5. QEMU 设置 `env->pc=upcall_entry`；
6. QEMU 调用 `cpu_loop_exit_noexc()`；
7. EL0 assembly 保存全部 application context；
8. EL0 dispatcher acquire-load producer sequence 并处理 event；
9. EL0 release-store consumer sequence；
10. EL0 scheduler 选择 READY coroutine 并执行 resume assist。

completion 到达且另一个 coroutine 正在运行时，completion 保留在 device event
model 中。下一个 EL0 TB boundary 将 descriptor 发布到 ring 并触发 direct upcall。

## 6. All-blocked 时序

EL0 scheduler 找不到 READY coroutine 且仍存在 `WAIT_REMOTE` context 时：

1. drain event ring；
2. 执行 wait assist；
3. wait helper 发现已有 event 时直接发布并返回；
4. wait helper 发现空队列时挂起 vCPU；
5. remote completion 生成 event；
6. QEMU 发布 descriptor 和 producer sequence；
7. QEMU kick home vCPU；
8. EL0 从 wait assist 下一条指令继续；
9. scheduler drain event ring 并恢复 READY coroutine。

## 7. Kernel ABI 责任

`QUERY_CAPS` 返回：

- ABI version 3；
- event slot bytes；
- event ring mmap offset/bytes；
- consumer mmap offset/bytes；
- resume/wait/scheduler-enter HLT immediate；
- `KERNEL_FREE_EVENT_RING` 与 `EL0_WAIT_WAKE` capability。

driver open 阶段分配并清零两个 coherent mapping。driver mmap 规则：

- producer/event ring 只允许 `PROT_READ | MAP_SHARED`；
- consumer page 允许 `PROT_READ | PROT_WRITE | MAP_SHARED`；
- mapping 长度和 offset 必须与 caps 完全一致。

`START` 将两个 DMA address、bytes、depth、owner generation 和 upcall entry 配置到
QEMU device，然后启用 load interception。`STOP` 先停 interception 并回收 pending
state，再释放 session。mapping 在 file release 时释放。

## 8. Test contract

静态 contract 必须断言：

- runtime hot path 不包含 `OBMM_ASYNC_LOAD_IOCTL_GET_EVENT`；
- runtime hot path 不包含 `OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER`；
- runtime 映射 producer/event ring 与 consumer page；
- assembly 包含 resume、wait、scheduler-enter 三个 assist；
- QEMU direct delivery 在 event publish 成功后改写 PC并退出 TB；
- driver 的 async-load file operations 包含 mmap；
- producer mapping 拒绝 writable VMA。

2-node E2E 日志必须包含：

- `abi=3`；
- `event_delivery=ring`；
- `kernel_hotpath_ioctls=0`；
- PENDING 与 COMPLETE sequence；
- 两个 coroutine 的 switch/resume；
- patch/replay 的最终 value 校验；
- wait/wakeup counter；
- QEMU、driver 与 EL0 runtime 的 producer/consumer 最终值相等。

## 9. 实现落点

| 层 | 主要文件 | ABI v3 责任 |
| --- | --- | --- |
| UAPI | `guest-linux/kernel_ub/include/uapi/ub/obmm_async_load.h` | 固化 ring header、128-byte descriptor、consumer page、caps/start v3 与三个 HLT immediate |
| guest driver | `guest-linux/aarch64/driver/linqu_ub_drv.c` | 分配 coherent ring/page，实施只读/读写 mmap 权限，START 时向 QEMU 交付 DMA 地址 |
| QEMU device/model | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load*.c` | descriptor-first publish、sequence/owner/capacity 校验、completion wakeup 与 fail-stop |
| AArch64 TCG | `vendor/qemu_8.2.0_ub/target/arm/tcg/{translate-a64,helper-a64}.c` | direct-upcall TB exit，以及 resume/wait/scheduler-enter 三个 EL0 assist |
| EL0 runtime | `guest-linux/aarch64/libs/obmm_coroutine_scheduler/` | acquire 消费、event state transition、release ack、协程调度与热路径计数 |
| E2E app/gate | `guest-linux/aarch64/apps/obmm_async_coroutine/`、`run_ub_obmm_eval.sh` | 2-node producer/consumer 因果验证及 ABI v3 machine-readable gate |
| P3 evaluator | `crates/sim-cli/src/{obmm_remote,obmm_eval}.rs` | 生成 `v3|...` 模型 spec，拒绝缺失 ring/wait/wakeup 证据的 async-load 样本 |

## 10. 2-node 实跑结果

2026-08-31 在 `n4-910c` 的隔离工作区
`/home/ll/ub_sim_abi_v3_20260831` 顺序运行 patch 与 replay。两个 run 使用完全相同的
QEMU、kernel、initramfs、scenario 与 remote-memory manifest：

| artifact | SHA-256 / contract hash |
| --- | --- |
| QEMU | `5374ab0046e70a335681edb5684209aa3fbb1f4b0551b4d58da1a9e600040ec9` |
| kernel Image | `4aea7db353ce48baf9f1416f5d728a888cb402f79d19af978e2d8aa5532f8a35` |
| initramfs | `0feec129e110e13ab10818a667b8e6f06004e173a07f3051cd954f6044317884` |
| 10 ms scenario | `dde4d793e72725d1f0c2effc1b777fa07968a48c8f02187118bf113092671009` |
| model file | `702bb3b440b9aa555b1c72dd9e39173edf10225a82d11ec57d16e52282543035` |
| model contract | `fnv1a64:790d2188d12e4513` |

共同结果：

| 验收项 | patch | replay |
| --- | ---: | ---: |
| nodeA writes / nodeB verified values | 2 / 2 | 2 / 2 |
| PENDING / COMPLETE / FAULT | 2 / 2 / 0 | 2 / 2 / 0 |
| event ring consumed | 4 | 4 |
| final producer / consumer sequence | 4 / 4 | 4 / 4 |
| EL0 wait assists / actual wakeups | 4 / 2 | 4 / 2 |
| scheduler-enter assists | 2 | 2 |
| hot-path ioctls | 0 | 0 |
| QEMU context saves/restores/switches/bytes | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| replay consumed / mismatch / ready high-water | 0 / 0 / 0 | 2 / 0 / 1 |
| blocked-load causal switches | 1 | 1 |
| QEMU remaining after cleanup | 0 | 0 |
| terminal status | pass | pass |

本机留存的完整 runner、guest 与 QEMU 日志位于：

- `out/abi-v3-event-ring/abi-v3-ring-patch-20260831-r1.log`；
- `out/abi-v3-event-ring/abi-v3-ring-replay-20260831-r1.log`；
- `out/abi-v3-event-ring/*_headless8/{nodeA,nodeB}_{guest,qemu}.log`。

以上运行构成功能正确性证据。10 ms 固定延迟用于稳定暴露协程重叠、all-blocked wait
和 wakeup，不能解释为性能结果。P3 全矩阵继续保持暂停；恢复 P3 时必须使用 ABI v3
gate 和新 artifact 重新建立正式性能证据，ABI v2 的历史样本不能进入 ABI v3 聚合。

## 11. 回归结果

| 验证 | 环境 | 结果 |
| --- | --- | --- |
| `cargo test --workspace` | `n4-910c` ARM64 Linux | pass；所有 workspace unit/doc tests 完成，外部模型或 native runtime 依赖项按测试声明 ignored |
| `python -m unittest discover guest-linux/aarch64/tests` | `n4-910c`，Python 3.12.14 | 358/358 pass |
| async-load focused Python contract | macOS 与 `n4-910c` | 20/20 pass |
| QEMU async-load / remote-model / backend unit | `n4-910c` ARM64 native | 9 + 7 + 6 = 22/22 pass |
| QEMU wrapper build | macOS 与 `n4-910c` ARM64 native | pass |
| guest kernel、driver、initramfs build | `n4-910c` ARM64 Linux | pass |
| 2-node patch producer/consumer | `n4-910c` real QEMU guest | pass |
| 2-node replay producer/consumer | `n4-910c` real QEMU guest | pass |
| post-run QEMU residual check | `n4-910c` | 0 |

远端系统默认 Python 3.9 无法执行仓库中已使用 PEP 604 union 与
`Path.write_text(newline=...)` 的测试源码。本次全量 Python 回归使用隔离的 Python
3.12.14 环境 `/home/ll/.local/envs/ub-sim-py312`，没有修改测试以适配旧解释器。
