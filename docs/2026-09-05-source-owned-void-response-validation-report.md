# Source-owned void response 实现与验证报告

日期：2026-09-05

验证状态：通过

验证范围：两节点 QEMU、Normal NC、Normal Cacheable、source-local trigger、remote-wire
trigger、双 trigger 仲裁

## 1. 结论

本轮实现完成了 source-owned void-response contract：

- 发出 remote read 并持有 outstanding transaction 的 source UBC，是唯一执行
  CPU-facing void raise 的组件；
- source-local policy 命中和 destination 返回 wire-level UB VOID 均进入 source UBC
  的同一 transaction 状态迁移；
- 每笔 transaction 最多执行一次 `ACTIVE → VOIDED` 和一次 precise Data Abort；
- Data Abort handler 保持 faulting task 为 runnable，调用 `schedule()` 后经标准 `ERET`
  返回原 `LDR`；
- replayed `LDR` 发起新 UB transaction；
- voided 旧 transaction 的 real completion 在 source UBC 内丢弃并清理；
- 软件路径没有 wait key、event ring、completion CQ/IRQ、task sleep 或 completion wakeup；
- Normal NC 没有分配 PLT；Normal Cacheable 只在 replay transaction 成功后执行普通
  cache-line fill。

六个两节点 acceptance case 全部通过。每个 case 使用两个 Linux task，各执行一笔普通
8-byte `LDR` 并验证 exporter 写入的值。所有 case 都得到 `verified=2/2`、
`protocol_errors=0`、`timeouts=0` 和 `qemu_destroyed=1`。

## 2. 实现落点

### 2.1 可调 predicate 模块

文件：

- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_void_response_policy.h`
- `vendor/qemu_8.2.0_ub/hw/ub/ub_void_response_policy.c`
- `vendor/qemu_8.2.0_ub/tests/unit/test-ub-void-response-policy.c`

配置 contract：

```text
v1|enabled=0|1|threshold_ns=N|latency_ns=N|jitter_ns=N|fault_voids=N|seed=N
```

policy 输出 `send_void`、effective completion delay、deterministic jitter 和 reason。
`fault_voids=N` 强制前 N 次 eligible decision 选择 void，随后恢复到 threshold 判定。
同一个 API 分别绑定到 source UBC 和 destination UBC；两个 binding 持有独立 policy
state 与 counters。

当前 source-local decision 在 remote request 发送后立即发生。decision 使用配置模型，
尚未读取实时 queue occupancy 或 transaction 实际已等待时间。

### 2.2 UB wire 与 source transaction

主要文件：

- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`
- `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_obmm_remote.h`

destination remote-side binding 使用：

- request flag：`UBC_SIM_DEC_READ_FLAG_VOID_ELIGIBLE`；
- response status：`UBC_SIM_DEC_READ_STATUS_VOID`；
- source 内部 response identity：`{peer_cna, req_id}`。

source 为每笔 remote read 创建 `UbcObmmAsyncChild`。其中 `void_trigger` 记录仲裁胜者：

```text
UBC_VOID_TRIGGER_SOURCE_POLICY
UBC_VOID_TRIGGER_REMOTE_WIRE
```

`ubc_obmm_async_child_try_void()` 只接受 active 且尚未看到 first response 的 transaction。
成功调用会同时设置 `first_response_seen`、`voided` 与 `void_trigger`，随后形成唯一
`UB_VOID_RESPONSE_CPU_RAISE`。后到的 trigger 形成
`UB_VOID_RESPONSE_TRIGGER_RACE_LOST`。

voided transaction 的 real response 只设置内部 `late_real_seen`。bottom half 打印
`UB_VOID_RESPONSE_LATE_DROP`，调用只承担 future/record 回收的内部 callback，然后清空
transaction record。该路径不复制 payload，也不 raise async-load IRQ。

### 2.3 Core fault 与 Linux task

主要文件：

- `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load_device.c`
- `guest-linux/aarch64/driver/linqu_ub_drv.c`
- `guest-linux/kernel_ub/include/uapi/ub/obmm_async_load.h`
- `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c`

QEMU capability bit `OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY` 表示 tokenless
void-response replay contract。source UBC 的 void result 经 CPU helper 形成 precise remote
load Data Abort。driver handler 使用异常现场的 `current`，保持 `TASK_RUNNING`，调用
`schedule()`，随后返回。EL1 没有登记 key 或 waiter。

guest app 的机器可读 summary 在该 capability 下固定报告：

```text
event_delivery=void-response-data-abort
scheduling=runnable-yield
retirement=replay
late_completion=drop
pending=0 completions=0 sleeps=0 wakeups=0
```

### 2.4 CLI 与 acceptance gate

主要文件：

- `guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh`
- `guest-linux/aarch64/scripts/run_ub_obmm_eval.sh`
- `guest-linux/aarch64/scripts/launch_ub_four_node_headless.sh`
- `guest-linux/aarch64/scripts/launch_ub_eight_node_headless.sh`
- `guest-linux/aarch64/tests/test_obmm_async_contract.py`
- `guest-linux/aarch64/tests/test_obmm_async_load_coroutine_contract.py`

CLI 提供两个独立配置：

```text
--void-response-policy SPEC
--source-void-response-policy SPEC
```

前者控制 destination remote-side binding，后者控制 source-local binding。runner gate
按 trigger 原因分别统计 source raise，并验证 decision、wire TX/RX、race-lost、late
drop、runnable yield、task result 和 QEMU cleanup 的因果关系。

## 3. 验证环境与 artifact

验证主机：`n4-910c1`

隔离 workspace：`/home/ll/ub_sim_void_response_20260905`

scenario：`scenarios/mvp_2host_async_load_remote_10ms.yaml`

统一 artifact fingerprints：

| artifact | SHA-256 |
|---|---|
| QEMU | `bf676bba7d799b324bc9fc1afeb786596824b9823242d2b29c2eb2384d8d357f` |
| kernel Image | `f3788367dfb0e926c687bdaf2ebfa1e1494280874a0a9003d428d6d0e12ad279` |
| initramfs | `f98b9b4fe8eff5659a4bb0e9fe3e4323e38bbbe0c534b1254ae134741cf354b3` |
| scenario | `dde4d793e72725d1f0c2effc1b777fa07968a48c8f02187118bf113092671009` |
| remote model file | `22906130f5e65f4b9956697c1d6d21393477d6c5f1fe5931e2ad6eb5a7fcdcd1` |

remote model contract hash：`fnv1a64:d3eea69fc6d04399`。

提交关系：

| repository | revision | 内容 |
|---|---|---|
| root design | `d1263c1` | source-owned 设计文档与 SVG |
| QEMU nested repo | `f201ebe131` | predicate、wire、transaction 仲裁与 CPU fault delivery |
| kernel nested repo | `85fb35d4ec75` | void-response retry capability UAPI |

共同 async-load model：

```text
v3|enabled=1|contexts=64|pending=64|events=128|clock_mhz=2000
```

共同 predicate config：

```text
v1|enabled=1|threshold_ns=2000|latency_ns=1000|jitter_ns=0|fault_voids=2|seed=1
```

每例的 `fault_voids=2` 让两个首次 transaction 进入 void path；两个 replay transaction
得到 real response，用于验证恢复后可完成和数据正确性。

## 4. 单元与 contract 测试

QEMU 使用项目 wrapper 构建：

```text
cd guest-linux/aarch64
./scripts/build_qemu_binary.sh --with-obmm-tests
```

| suite | 结果 | 覆盖重点 |
|---|---:|---|
| `test-ub-obmm-remote` | 6/6 | transaction、capacity、timeout、late sink |
| `test-ub-obmm-remote-model` | 7/7 | model、timer、drop、duplicate、manifest |
| `test-ub-void-response-policy` | 5/5 | disabled、threshold、fault recovery、jitter、非法配置 |
| `test-ub-async-load` | 11/11 | fault、replay、capacity、Cacheable bypass |
| 合计 | 29/29 | pass |

根仓库 Python contract suite 的结果记录在第 7 节。

## 5. 两节点 acceptance 矩阵

### 5.1 结果

| trigger | memory | run ID | raises L/R | decision S-void/S-wait | decision D-void/D-real | TX/RX | race | drops | verified |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| source-local | NC | `void-source-local-nc-20260905-r2` | 2/0 | 2/2 | 0/0 | 0/0 | 0 | 2 | 2/2 |
| source-local | Cacheable | `void-source-local-cacheable-20260905-r2` | 2/0 | 2/2 | 0/0 | 0/0 | 0 | 2 | 2/2 |
| remote-wire | NC | `void-remote-wire-nc-20260905-r2` | 0/2 | 0/0 | 2/2 | 2/2 | 0 | 2 | 2/2 |
| remote-wire | Cacheable | `void-remote-wire-cacheable-20260905-r2` | 0/2 | 0/0 | 2/2 | 2/2 | 0 | 2 | 2/2 |
| dual-trigger | NC | `void-dual-trigger-nc-20260905-r2` | 2/0 | 2/2 | 2/2 | 2/0 | 2 | 2 | 2/2 |
| dual-trigger | Cacheable | `void-dual-trigger-cacheable-20260905-r2` | 2/0 | 2/2 | 2/2 | 2/0 | 2 | 2 | 2/2 |

`raises L/R` 分别表示 source-policy 和 remote-wire 成为仲裁胜者后产生的 source UBC
raise 数。双 trigger 场景中 source-local trigger 先完成状态迁移；两份后到 wire VOID
都进入 race-lost，CPU raise 总数仍为 2。

### 5.2 共同机制 gate

六例全部满足：

| gate | 结果 |
|---|---:|
| producer writes | 2 |
| consumer operations / verified | 2 / 2 |
| Data Abort faults / runnable yields | 2 / 2 |
| late real drops | 2 |
| kernel blocks / wakes | 0 / 0 |
| summary pending / completions / sleeps / wakeups | 0 / 0 / 0 / 0 |
| NC PLT allocations / pending | 0 / 0 |
| protocol errors / timeouts / interrupted waits | 0 / 0 / 0 |
| direct EL0 upcalls | 0 |
| QEMU cleanup | pass |

三个 Cacheable case 还满足 `cacheable_fill_bytes=128` 和
`cacheable_replay_hits=2`。这些数据来自两笔成功 replay transaction。被 void 的旧
transaction payload 没有进入 HA fill。

### 5.3 数据正确性

producer 写入：

```text
offset=4096 value=3f2b79b97f4a7c15
offset=8192 value=3f2b79b97f4a7c14
```

每个 case 的两个 consumer task 都读取到对应值，最终 checksum 均为
`b034970d6e5bae14`。

### 5.4 日志位置

每个 run 的证据位于：

```text
/home/ll/ub_sim_void_response_20260905/guest-linux/aarch64/logs/
  <run-id>_headless8/control.log
  <run-id>_headless8/nodeA_qemu.log
  <run-id>_headless8/nodeA_guest.log
  <run-id>_headless8/nodeB_qemu.log
  <run-id>_headless8/nodeB_guest.log
```

runner 终端输出中的 `OBMM_ASYNC_LOAD_VOID_RESPONSE_EVIDENCE ... status=pass` 是本轮
acceptance 的机器可读因果 gate。

## 6. 验证说明

本轮 latency 字段只用于确认 guest 能形成 fault、yield、replay 与成功读取的闭环。
QEMU guest 单例、两次操作和 event logging 打开的条件不适合形成性能结论。P3 性能矩阵
没有在本轮恢复。

双 trigger 结果验证了 QEMU 模型中的 exactly-once 行为。QEMU event loop 对当前状态
迁移提供串行执行环境；silicon 实现仍需对 transaction 的 `ACTIVE → VOIDED` 转换提供
原子状态保护。

## 7. 最终回归结果

| gate | 执行位置 | 结果 |
|---|---|---|
| QEMU wrapper rebuild | n4-910c1 | pass |
| QEMU 4 suites | n4-910c1 | 29/29 pass |
| `python3 -m unittest discover guest-linux/aarch64/tests` | n4-910c1 | 391/391 pass |
| `cargo test --workspace` | n4-910c1 | pass；0 failed |
| modified zsh entrypoint syntax | local | pass |
| 4 个 SVG 的 XML parse | local | pass |
| root/QEMU/kernel `git diff --check` | local | pass |
| `cargo fmt --all -- --check` | local | pass |
| residual `qemu-system-aarch64` | n4-910c1 | 0 |

Python suite 首次从非交互 SSH 运行时有 4 项 W5 validate-only case 失败，直接原因是该
session 的 `PATH` 不含 `/home/ll/.cargo/bin`。在 `PATH` 加入服务器现有 cargo 安装目录
后，完整 391 项重跑通过。首次失败没有涉及 async-load behavior 或 source code defect。

Rust workspace 的一次并行回归在
`qwen3_prepare_engram_simt_mode_runs_launch_self_check` 中遇到
`Text file busy (os error 26)`。该用例会创建并立即执行临时 ENGRAMSIMT
artifact。单用例使用 `--test-threads=1` 重跑通过，随后完整 workspace
使用同样的串行配置重跑通过，最终结果为 0 failed。这个现象属于测试
artifact 写入与执行之间的环境竞争，本轮 async-load 改动没有修改该用例。

n4-910c1 的 stable Rust toolchain 未安装 `rustfmt` component，远端 format check 因
环境缺件退出。本地对同一 workspace 执行 format check 通过；本轮没有修改 Rust source。

## 8. 已知边界

- acceptance 覆盖 2 nodes、2 tasks、每 task 1 次成功 load；多 requester/core 深队列和
  长时间持续 void 需要压力测试；
- policy 在 QEMU realize 时解析，运行中没有 QMP/MMIO update；
- source-local binding 依赖配置的 synthetic latency/jitter 与 fault counter；实时 source
  queue occupancy、retry budget 和已等待时间尚未接入；
- destination model 在 wire VOID 后仍产生 real response，本轮没有注入最终 remote error
  status；
- fault reason 使用项目的 implementation-defined remote-load Data Abort contract；硬件
  架构分配和 Linux upstream 接口仍需单独评审；
- transaction record 与 policy timer 属于 QEMU simulation model。silicon contract 需要
  transaction identity、first-response state、predicate decision、wire VOID 和 late
  response discard；软件不观察这些内部记录。
