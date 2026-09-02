# Normal Cacheable async `LDR` 的 void response、ESR 与 CQ/IRQ 验证设计

日期：2026-09-02

状态：实现完成；two-node arm64 guest acceptance 已通过

## 1. 结论

Normal Cacheable remote load 需要单独验证完整的软件可观察链路：

1. EL0 `LDR` 产生普通 cache miss，requester UBC 接收远端请求；
2. UBC 立即发布 `REMOTE_PENDING`，CPU 以精确 lower-EL Data Abort 进入 EL1；
3. `ESR_EL1`、`FAR_EL1`、`ELR_EL1` 携带 fault reason、VA 与原 `LDR` PC；
4. Linux fast path 将当前 task 绑定到 `wait_key` 并进入 waitqueue，Linux scheduler
   运行其他 runnable task；
5. UBC 收到远端响应后把数据交给普通 cache fill 路径；
6. fill 完成后发布 completion CQE，再触发 IRQ；
7. driver drain CQ、按 `wait_key` 唤醒原 task；
8. fault handler 返回，标准异常返回路径执行 `ERET`；
9. 原 `LDR` 从原 PC replay，并命中已经填充的数据。

这条路径全程禁止分配 NC PLT。验收日志必须给出
`nc_plt_allocations=0`、`nc_plt_pending=0`，同时给出 PENDING、ESR fault、task sleep、
CQ completion、IRQ wakeup、task resume 和 replay hit 的计数。

![Normal Cacheable void response 与 CQ/IRQ 验证链路](./2026-09-02-normal-cacheable-void-response-esr-cq-validation.svg)

## 2. 与 Normal NC 的状态边界

| 项目 | Normal Cacheable | Normal Non-cacheable |
|---|---|---|
| outstanding memory state | 普通 cache/MSHR/fill state | requester UBC NC PLT |
| `wait_key` 来源 | 普通 miss transaction identity | NC PLT slot/generation token |
| remote response data | 完整 fill unit 进入普通 cache | scalar value 进入 NC PLT |
| completion CQE | readiness、status、`wait_key` | readiness、status、NC replay token |
| resume 前的 device command | 无 NC token arm | arm one-shot NC replay token |
| 原 `LDR` replay | 普通 cache hit | 精确消费 NC PLT result |
| NC PLT allocation | 恒为 0 | 每条 outstanding dynamic load 一项 |

`wait_key` 只承担 software wait/wakeup correlation。它不保存 load value、cache line、
register context 或 scheduler state。Normal Cacheable 的 `wait_key` 生命周期止于
completion CQE 被 driver 消费并唤醒目标 task。

## 3. 硬件组件职责

### 3.1 CPU core

- 保留 faulting `LDR` 的 PC；
- 生成精确 Data Abort；
- 在 `ESR_EL1` 中编码 `REMOTE_PENDING` 与 access attributes；
- 使用现有 exception entry 与 `ERET`；
- task 恢复后重新执行原 `LDR`。

CPU core 不增加 remote-specific pending-load table。

### 3.2 普通 cache 与 MSHR

- 按普通 Normal Cacheable miss 分配 MSHR；
- 保存 line address、fill ownership、merge waiter 与 coherence state；
- remote response 到达后完成普通 fill；
- replay `LDR` 通过普通 tag/data lookup 命中。

MSHR 不保存 task/coroutine ID、PC、SP、`Rt`、saved context、CQ ownership 或
software scheduler state。local 与 remote line 采用相同 entry 格式和 fill contract。

### 3.3 Requester UBC

- 对 accepted remote miss 立即产生 void response；
- 维护 transport transaction 到普通 fill transaction 的关联；
- 收到数据后将完整 fill unit 交给 cache hierarchy；
- fill 可观察完成后发布 CQE 和 IRQ。

UBC 不保留 replay-ready scalar result，也不为 Cacheable load 分配 NC PLT。

### 3.4 Linux driver 与 scheduler

- 通过 PENDING event 将 `{wait_key, task cookie, PC, VA}` 会合；
- fault handler 将 task 放入 waitqueue；
- completion IRQ handler drain CQ 并按 `wait_key` 唤醒 task；
- Cacheable success 路径直接返回 fault handler；
- Linux exception exit 使用标准 `ERET` 回到原 PC。

## 4. QEMU 仿真边界

QEMU TCG 没有可用于本项目的 timing-accurate CPU cache/MSHR hierarchy。功能验证采用
以下映射：

| silicon contract | QEMU 功能模型 |
|---|---|
| PTE memory type | 从 arm64 TLB full entry 的 `pte_attrs` 识别 Normal Cacheable |
| ordinary miss identity | async backend future 的 generation/slot，仅保留 transport 生命周期 |
| immediate void response | 在 remote submit 前发布 PENDING event，随后 raise Data Abort |
| ordinary fill | 复用现有 sim-decoder page-cache，并增加普通 cache-line valid bitmap |
| fill hit | replay helper 查询同一普通 page-cache；line valid 后放行 translated load |
| completion CQ/IRQ | 复用 ABI event ring、producer sequence 与 async-load IRQ |

page-cache surrogate 已经存在于 sim-decoder。本次在现有 entry 内增加 64-bit line-valid
bitmap，使 4 KiB page entry 可以表达 64 个独立的 64-byte line。该模型不进入 silicon
contract，也不增加硬件 remote line fill table。QEMU campaign 与当前硬件目标都使用
64-byte fill unit。该 campaign 可以证明 exception、schedule、CQ/IRQ、wakeup 与 replay
ordering；真实 cache timing、MSHR pressure 与 coherence 性能仍需 timing model 或 RTL
验证。

## 5. 时序与 ordering

### 5.1 PENDING

固定顺序为：

`future reserved → PENDING event visible → remote request submitted → Data Abort raised`。

PENDING event 必须先于 fault handler drain event ring。remote submit 失败时发布 terminal
FAULT，fault handler 按正常错误路径结束。

### 5.2 Completion

成功路径固定顺序为：

`remote payload complete → cache fill visible → COMPLETE CQE visible → producer sequence
visible → IRQ asserted → driver drain CQ → wake_up(task)`。

driver 在观察到 COMPLETE 之前不能唤醒 task。task 被唤醒后无需 arm NC replay token。

## 6. CLI 与验收用例

`obmm_async_coroutine` 增加：

```text
--async-load-memory normal-nc|normal-cacheable
```

首个 Normal Cacheable acceptance 使用：

```text
--mode async-load --kernel-task-replay \
--async-load-memory normal-cacheable \
--threads 2 --iterations 2 --access-bytes 8 \
--pattern sequential --verify
```

QEMU 启动环境启用已有 sim-decoder page-cache capacity。两条线程访问不同 page，
producer 预先写入两个不同的 64-bit value。

必须同时满足：

- 2 个值均等于 producer 写入值；
- `faults=2`、`pending=2`、`completions=2`；
- `task_sleeps=2`、`task_wakeups=2`；
- `cacheable_fill_pending=2`、`cacheable_fill_completed=2`；
- `cacheable_replay_hits>=2`；该 counter 统计 helper 观察到的 line-valid lookup，不能用作
  architectural retirement counter；
- `nc_plt_allocations=0`、`nc_plt_pending=0`；
- `nc_replay_consumed=0`、`replay_mismatch=0`；
- `protocol_errors=0`、`timeouts=0`；
- QEMU 日志包含 Cacheable PENDING、64-byte fill 与 replay-hit evidence；guest 日志包含
  `FSC=0x3a`、CQ completion、IRQ handler 与 `resume=eret-replay`；
- campaign 结束后没有残留 QEMU process。

## 7. 实现位置

| 层次 | 文件 | 本次职责 |
|---|---|---|
| AArch64 helper | `vendor/qemu_8.2.0_ub/target/arm/tcg/helper-a64.c` | 读取 PTE attributes，识别 Normal Cacheable，并生成 implementation-defined Data Abort |
| async-load device | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load_device.c` | Cacheable future、PENDING-before-submit、fill-before-completion、CQ/IRQ 与 replay lookup |
| async-load model | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load.c` | Cacheable event 与 path counters；Cacheable API 不访问 NC PLT |
| UBC / fill surrogate | `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` | 64-byte remote line fill、普通 page-cache line-valid bitmap |
| kernel UAPI | `guest-linux/kernel_ub/include/uapi/ub/obmm_async_load.h` | capability、event flag、path-stats ioctl |
| Linux driver | `guest-linux/aarch64/driver/linqu_ub_drv.c` | FSC `0x3a` fault fast path、waitqueue、CQ/IRQ drain、ERET replay return |
| guest CLI | `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c` | `--async-load-memory normal-cacheable`、两线程 value 与 counter gate |
| acceptance runner | `guest-linux/aarch64/scripts/run_ub_obmm_eval.sh` | artifact fingerprint、ESR/CQ/replay/PLT-zero 因果门禁、QEMU 清理 |

## 8. 2026-09-02 n4-910c1 验证结果

隔离工作目录：
`/home/ll/ub_sim_normal_cacheable_20260902_r1`。

正式通过的 campaign：`cacheable-esr-cq-20260902-r4`。
证据目录：
`guest-linux/aarch64/logs/cacheable-esr-cq-20260902-r4_headless8/`。

### 8.1 two-node result

| 检查项 | r4 结果 |
|---|---:|
| producer writes / consumer verified values | 2 / 2 |
| `ESR_EL1` FSC `0x3a` | 2 |
| PENDING / block / completion / wake | 2 / 2 / 2 / 2 |
| successful `resume=eret-replay` | 2 |
| 64-byte line fill pending / completed | 2 / 2 |
| total fill bytes | 128 |
| Cacheable line-valid lookup hits | 4 |
| NC replay consumed / mismatch | 0 / 0 |
| NC PLT allocations / pending | 0 / 0 |
| protocol errors / timeouts / interrupted waits | 0 / 0 / 0 |
| backend duplicate / late | 0 / 0 |
| PENDING before remote submit | pass；2 PENDING / 2 single-line submits |
| QEMU residual processes | 0 |
| terminal status | pass |

`cacheable_replay_hits=4` 来自两条 replay LDR 各自触发两次 line-valid lookup。该 counter
用于证明 replay 期间数据已经可见；`operations=2`、`verified=2`、两条 thread terminal
record 与原值相等，共同构成 architectural result 证据。

关键因果日志如下：

```text
ASYNC_LOAD_CACHEABLE_PENDING ... fill_bytes=64 nc_plt_pending=0
remote-load block path=normal-cacheable ... esr=0x93c0803a fsc=0x3a
ASYNC_LOAD_CACHEABLE_FILL ... fill_bytes=64 nc_plt_pending=0
remote-load completion path=normal-cacheable ... status=0
remote-load wake path=normal-cacheable ... resume=eret-replay result=0
OBMM_ASYNC_LOAD_KERNEL_THREAD ... expected=<producer-value>
  actual=<producer-value> status=pass
```

### 8.2 失败尝试与修正

| campaign | 结果 | 根因与处理 |
|---|---|---|
| r1 | fail | functional fill 误用 4 KiB，单个 miss 占满 64-entry remote queue；改为 64-byte cache-line fill |
| r2 | mechanism pass、gate fail | 完整链路和 value 均正确；gate 把 line-valid lookup 错当成精确 retirement counter；改为下界门禁，并由 thread result 判定 retirement |
| r3 | pass | 64-entry queue 保持不变；两个并发 miss、ESR/CQ/IRQ/ERET replay、value 与 PLT-zero gate 全部通过 |
| r4 | pass | 增加 `PENDING → remote submit → fill` 顺序门禁；完整 acceptance 再次通过 |

### 8.3 artifact fingerprint

| artifact | SHA-256 |
|---|---|
| QEMU | `cb4473a3786415791ad7861f7a654ae003eab7b286a59264e0f252e01281a45a` |
| kernel `Image` | `9ae98e19d8b0577a23222bb6773484dcdba1caec231fd846b2a7a0b3bd667757` |
| initramfs | `da9d2dc87af447150de1c7f4b71b89fc5f08d22ad0c943e58be0d80f01ba4871` |
| `linqu_ub_drv.ko` | `8621b4fd46432c2290bc3cd871a0602e613b47a45e0f5fafbf3a687cb24beeda` |
| model manifest | `22906130f5e65f4b9956697c1d6d21393477d6c5f1fe5931e2ad6eb5a7fcdcd1` |
| scenario | `dde4d793e72725d1f0c2effc1b777fa07968a48c8f02187118bf113092671009` |

### 8.4 regression

| 验证 | 结果 |
|---|---|
| QEMU async-load unit | 11/11 pass |
| QEMU remote backend / model unit | 6/6、7/7 pass |
| QEMU full wrapper build | pass |
| guest kernel、driver、initramfs build | pass |
| async-load focused contract | 16/16 pass |
| guest Python contract suite | 390/390 pass |
| Rust workspace | pass；`sim-uapi` heavy group 159 pass、9 ignored |
| `cargo fmt --all -- --check` | pass |
| SVG XML validation | pass |
| Normal NC shared-future two-node regression | `nc-future-reg-20260902-r2` pass；2 values、2 PLT allocations、2 replay consumes |

## 9. 尚未由该用例证明的范围

- timing-accurate cache-line fill latency；
- real MSHR merge、capacity、eviction 和 backpressure；
- Cacheable remote write 与 invalidation；
- coherent shared-writer workload；
- cacheline crossing load；
- timeout、cancel、late completion；
- EL0 coroutine 的 Normal Cacheable direct-upcall 版本。

这些范围需要后续专用模型与 failure matrix。当前 acceptance 只对 read-only、producer
发布后保持稳定的 remote data 做功能判定。
