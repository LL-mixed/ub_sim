# UB void-response predicate policy 设计与验证

日期：2026-09-05
状态：实现完成；Normal NC 与 Normal Cacheable 两节点 acceptance 通过

## 1. 目标与边界

destination UBC 针对每笔 eligible remote read 独立判断：

- 预计可在 threshold 内完成：返回 real response；
- 预计延迟超过 threshold，或故障注入命中：先返回 UB void response；
- remote read 始终继续执行；
- voided transaction 的 real completion 到达 source UBC 后，只触发 payload drop 和
  transaction cleanup。

policy 只负责 decision 与模拟 completion delay。UB wire encoding、source transaction
tracking、Data Abort、scheduler yield 和 load replay 都位于 policy 模块之外。后续增加新
策略时，不需要复制 async-load data path。

CPU-facing delivery 遵守一条固定 ownership 规则：发出 UB 访存事务并持有 outstanding
transaction 的 source UBC，是唯一可以向 requester core raise void response 的组件。
void trigger 有两类来源：

| trigger | UB wire 行为 | CPU-facing raise |
|---|---|---|
| source UBC 本地策略命中 | 无需等待 wire VOID | source UBC 将 transaction 置为 VOIDED 后 raise |
| destination policy 命中 | destination 返回 wire-level UB VOID | source UBC 收到 VOID、将 transaction 置为 VOIDED 后 raise |

destination UBC 不直接访问 source core。当前 QEMU v1 已实现并验证第二条路径；第一条
路径属于下一步 source-local predicate 扩展。

## 2. 模块接口

实现文件：

- `vendor/qemu_8.2.0_ub/include/hw/ub/ub_void_response_policy.h`
- `vendor/qemu_8.2.0_ub/hw/ub/ub_void_response_policy.c`

输入 `UbVoidResponseRequest`：

| 字段 | 含义 | 当前 v1 用途 |
|---|---|---|
| `source_cna` | requester CNA | deterministic jitter key |
| `request_id` | 当前 UB read request ID | deterministic jitter key |
| `remote_address` | destination remote UBA | deterministic jitter key |
| `length` | read bytes | deterministic jitter key |
| `arrival_ns` | destination 接收时间 | 预留给窗口、trace 与队列策略 |

输出 `UbVoidResponseDecision`：

| 字段 | 含义 |
|---|---|
| `send_void` | 是否先发送 void response |
| `fault_injected` | 本次 decision 是否来自故障注入 |
| `jitter_ns` | 本次确定性正负 jitter |
| `completion_delay_ns` | `latency_ns + jitter_ns`，下限为 0 |
| `reason` | disabled、within-threshold、latency-threshold 或 fault-injection |

模块 counters 为 `evaluated`、`void_selected`、`normal_selected`、`fault_injected`。
reset 会清零 counters，因此 `fault_voids=N` 在新 session 中重新形成一次故障窗口。

## 3. v1 predicate

配置格式：

```text
off
```

或：

```text
v1|enabled=0|1|threshold_ns=N|latency_ns=N|jitter_ns=N|fault_voids=N|seed=N
```

decision 顺序：

1. policy disabled：选择 real；
2. 使用 `{seed, source_cna, request_id, remote_address, length}` 计算确定性 jitter；
3. 计算 effective completion delay；
4. `evaluated < fault_voids`：强制选择 void，reason 为 `fault-injection`；
5. effective delay 大于 threshold：选择 void，reason 为 `latency-threshold`；
6. 其余请求选择 real，reason 为 `within-threshold`。

同一配置和同一 request identity 始终得到相同 jitter，回归测试可稳定复现。当前故障模型
表达“前 N 笔 eligible request 故障，随后恢复”。周期窗口、概率故障、按地址范围故障和
外部 trace 驱动留给后续 schema 版本。

## 4. 与 remote-read data path 的连接

### 4.1 destination UBC

source 仅在 kernel-task ordinary-load 路径设置
`UBC_SIM_DEC_READ_FLAG_VOID_ELIGIBLE`。destination UBC 收到 request 后：

1. 验证 eligibility 和 policy enabled；
2. 调用 policy `decide()`；
3. `send_void=1` 时立即发送 status 为 `VOID` 的 UB read response；
4. 读取 destination memory 并获得 real payload/status；
5. 按 `completion_delay_ns` 使用 QEMU virtual timer 发送 real response。

显式 submit/poll load 请求不设置 eligibility，因此不会被 predicate 改写。

### 4.2 source UBC

source 为每笔请求保留 `UbcObmmAsyncChild` transaction record，以 `{peer_cna, req_id}`
匹配 response：

- first response 为 real：将 payload/status inline 返回当前 load；
- first response 为 wire-level void：record 进入 `voided`，source UBC 经 load helper
  向本地 requester core raise precise Data Abort；
- voided record 后续收到 real response：只设置 `late_real_seen`，BH 执行
  `OBMM_REMOTE_STATUS_VOIDED` callback 并回收 future/record；
- late payload 不复制到 guest memory、HA/cache、NC PLT、event ring 或 CQ；
- cleanup 不 raise IRQ，也不 kick core。

这里的 `req_id` 属于 UB transaction protocol。它不构成 software wait key，也不传给
ESR/EL1/task。

source-local predicate 接入后也必须进入相同的逻辑状态迁移：

```text
ACTIVE --source-local policy--> VOIDED --source UBC raises Data Abort
ACTIVE --remote wire VOID-----> VOIDED --source UBC raises Data Abort
```

每笔 transaction 只允许一次 `ACTIVE → VOIDED`。两个 trigger 发生竞争时，完成状态迁移
的一方负责 raise；后到的 VOID cause 只记录或丢弃，不再次通知 core。当前 QEMU v1 的
事件循环串行化 remote response 分支；硬件实现需要对该状态迁移提供原子保护。

### 4.3 EL1 与 replay

Data Abort handler 检测 capability
`OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY` 后执行：

1. 记录 fault counter；
2. 保持 `current` 为 `TASK_RUNNING`；
3. 调用 `schedule()` 让 scheduler 选择 runnable task；
4. handler 返回后通过标准 `ERET` 回到原 `LDR`；
5. 原 `LDR` 发起一笔新 transaction。

handler 不读取 key，不 drain event ring，不建立 waiter，不睡眠，也不等待旧 transaction
completion。

## 5. Cacheable 与 Normal NC

| 项目 | Normal Cacheable | Normal NC |
|---|---|---|
| request 到 source UBC | HA/cache miss path | 绕过 HA/cache |
| real response | 普通 line fill 后由 `LDR` 读取 | scalar value 返回当前 `LDR` |
| voided late payload | source UBC 丢弃，禁止 fill HA | source UBC 丢弃，禁止保存 scalar |
| replay | 新 transaction 成功后普通 fill/replay | 新 transaction 成功后直接返回 scalar |
| NC PLT | 0 | 0 |
| software wait key | 0 | 0 |
| completion CQ/IRQ | 0 | 0 |

## 6. CLI

两个入口均支持同一 option：

```text
--void-response-policy SPEC
```

- `guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh`
- `guest-linux/aarch64/scripts/run_ub_obmm_eval.sh`

4/8-node headless launcher 通过 `VOID_RESPONSE_POLICY` 接收 eval runner 透传，并向每个 QEMU
实例添加：

```text
-global ubc.void-response-policy=SPEC
```

示例：基准 1 us，threshold 2 us，正负 200 ns jitter，前两笔请求强制 void 后恢复：

```text
--void-response-policy \
  'v1|enabled=1|threshold_ns=2000|latency_ns=1000|jitter_ns=200|fault_voids=2|seed=1'
```

## 7. 可观测性

| log | 节点 | 含义 |
|---|---|---|
| `UB_VOID_RESPONSE_DECISION` | destination | remote-side request 输入、delay、jitter、action、reason |
| `UB_VOID_RESPONSE_TX` | destination | wire-level UB VOID 已发送；不直接通知 source core |
| `UB_VOID_RESPONSE_RX` | source | source transaction 已标记 voided，进入唯一 CPU-facing raise 入口 |
| `UB_VOID_RESPONSE_LATE_DROP` | source | late real completion 已丢弃并清理 |
| `remote-load void-response ... action=runnable-yield` | source guest | EL1 未阻塞 task，执行 scheduler yield |
| `OBMM_ASYNC_LOAD_VOID_RESPONSE_EVIDENCE` | acceptance runner | decision/TX/RX/drop/yield 因果计数 |

## 8. 验证结果

验证主机：`n4-910c1`
隔离目录：`/home/ll/ub_sim_void_response_20260905`

### 8.1 QEMU unit tests

| suite | 结果 |
|---|---:|
| `test-ub-obmm-remote` | 6/6 |
| `test-ub-obmm-remote-model` | 7/7 |
| `test-ub-void-response-policy` | 5/5 |
| `test-ub-async-load` | 11/11 |
| 合计 | 29/29 |

policy unit test 覆盖 disabled、threshold、fault/recovery、deterministic jitter 和非法 spec。

### 8.2 两节点 acceptance

共同 policy：

```text
v1|enabled=1|threshold_ns=2000|latency_ns=1000|jitter_ns=0|fault_voids=2|seed=1
```

| 项目 | Normal NC | Normal Cacheable |
|---|---:|---:|
| run ID | `void-predicate-nc-20260905-r2` | `void-predicate-cacheable-20260905-r2` |
| producer writes / consumer verified | 2 / 2 | 2 / 2 |
| void decisions / TX / RX | 2 / 2 / 2 | 2 / 2 / 2 |
| late drops / runnable yields | 2 / 2 | 2 / 2 |
| recovery 后 real decisions | 2 | 2 |
| pending / completion / sleep / wake | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| NC PLT allocations | 0 | 0 |
| Cacheable fill bytes / hits | 0 / 0 | 128 / 2 |
| protocol errors / timeouts | 0 / 0 | 0 / 0 |
| QEMU cleanup | pass | pass |
| terminal status | pass | pass |

artifact fingerprints 两个 run 一致：

- QEMU SHA-256：`f876f3d6c66af7aff6b29c235f3b3ef11c340024c1258d97ced7055662ec8cd3`
- kernel SHA-256：`f3788367dfb0e926c687bdaf2ebfa1e1494280874a0a9003d428d6d0e12ad279`
- initramfs SHA-256：`dfb39105b68076e1e5124360fcb12d0073ec1ba2bb5ab0f45c6757f60bebe507`

## 9. 已知限制与下一步

- policy 配置只在 QEMU realize 时解析，当前没有 QMP/MMIO runtime update；
- `fault_voids` 只影响 predicate 的 `send_void` decision；remote read 仍会产生最终
  real completion。该参数表达单个“forced-void 后恢复”窗口，当前没有最终 status
  故障、周期性故障或概率故障模型；
- policy stats 进入 QEMU log，当前没有独立 query command；
- predicate 使用配置的 latency/jitter 做决定，没有直接读取 destination queue occupancy；
- source-local predicate 尚未接入；当前 v1 通过 destination policy 返回 wire VOID，再由
  source UBC raise CPU-facing void；
- QEMU virtual timer/model/trace 属于 simulation-only 层；silicon contract 只要求
  eligibility、predicate decision、UB void response 和 source transaction cleanup；
- 多 requester/core 并发、queue-pressure predicate、长时间持续 void 的 fairness/backoff
  仍需专项验证。
