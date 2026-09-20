# Async load `wait_key` 历史方案说明

日期：2026-09-03。
状态：2026-09-05 起由 tokenless void-response replay contract 取代

归档复核：2026-09-20。本文解释旧 wait_key 方案退出主路径的原因。
现行设计见 [source-owned void response](2026-09-05-ub-void-response-predicate-policy-design.md)，
后续映射失效验证见 [服务补齐计划 M5](2026-09-08-gva-gsva-service-completion-plan.md)。
两张图保留原方案内容，均已标注历史状态，不作为当前接口或实现依据：

- [历史 wait_key 生命周期图](2026-09-03-async-load-wait-key-lifecycle.svg)
- [历史 fault-key 投递选项图](2026-09-03-async-load-fault-key-delivery-options.svg)

## 1. 结论

当前主方案不再创建、传递或消费 software-visible `wait_key`：

- source UBC 持有事务并仲裁本地策略触发与 destination wire VOID；
- source UBC 用 UB transaction identity 跟踪 wire transaction；
- void response 触发 precise Data Abort，EL1 根据 `current` 得到发出 `LDR` 的 task；
- fault handler 保持 task 为 runnable，执行一次 scheduler yield；
- `ERET` 返回原 `LDR` 后发起新 transaction；
- 旧 transaction 的迟到 real completion 在 source UBC 内丢弃并清理；
- software 不等待 completion，CQ、IRQ、event ring、NC PLT 和 replay latch 均不参与该路径。

因此，`wait_key_tag` 与 per-PE exception metadata register 两条 fault-key delivery
路线均退出当前实现范围。相关 SVG 只保留为历史讨论材料，不能作为当前 contract。

## 2. 当前使用的 identity

| identity | 维护组件 | 作用范围 | software 可见性 | 生命周期 |
|---|---|---|---|---|
| `{peer_cna, req_id}` | source/destination UBC | 单笔 UB read transaction | 不可见 | request 发出至 real response 或清理 |
| precise exception context | core/EL1 | 本次 faulting `LDR` | `current`、`ELR_EL1`、`FAR_EL1` | Data Abort 入口至 `ERET` |
| replay transaction identity | UBC | replay 后的新 UB read | 不可见 | 新 request 发出至其 response |

`req_id` 负责 UB response routing 和 source transaction lookup。它不承担 task lookup，
不写入 ESR，也不进入 driver 数据结构。

## 3. voided transaction 生命周期

| 阶段 | destination UBC | source UBC | EL1/software |
|---|---|---|---|
| request | 处理远端请求及配置的策略 | 创建 transaction record，评估 source-local 策略 | 原 task 执行 `LDR` |
| void | 可发送 wire-level void response | 仲裁本地触发或 wire VOID，最多一次转为 `VOIDED` | precise Data Abort |
| yield | remote read 继续进行 | 保留 record 等待收尾 | `TASK_RUNNING`，调用 scheduler |
| replay | 接收一笔新 request | 创建新的 transaction record | `ERET` 回原 PC，重新执行 `LDR` |
| old real completion | 发送旧 request 的 real response | 丢弃 payload，不通知 core，清理旧 record | 无感知 |
| new real completion | 返回新 request 的 data/status | Cacheable fill 或 NC scalar return | 新 `LDR` 正常退休 |

## 4. 为什么不需要 fault key

Data Abort 属于 precise synchronous exception。异常发生时，Linux `current` 就是执行该
`LDR` 的 task。当前 handler 不建立“completion 到 task”的异步关联，也不等待旧
transaction 完成，所以没有第二个事件需要通过 key 找回 task。

多 PE 同时向同一 UBC 发起 load 时，每个 void response 沿原 transaction response route
返回对应 requester。每个 PE 分别进入自己的 precise Data Abort；source UBC 依靠
`{peer_cna, req_id}` 匹配 wire response，EL1 依靠当前异常上下文处理当前 task。event
queue 顺序不参与匹配。

## 5. 历史方案何时仍有参考价值

如果未来重新引入以下任一语义，software-visible key 需要重新评审：

- task 在 void fault 后进入 sleeping，等待原 transaction completion 唤醒；
- completion 可在其他 PE 的 IRQ handler 到达，并要求精确唤醒原 task；
- Normal NC 要保存旧 transaction payload，随后由 replay `LDR` 消费；
- direct-EL0 coroutine 需要把异步 completion 投递给已挂起 coroutine。

这些语义不属于 2026-09-05 的 tokenless contract。

## 6. 当前实现位置

| 内容 | 文件 |
|---|---|
| destination predicate | `vendor/qemu_8.2.0_ub/hw/ub/ub_void_response_policy.c` |
| UB void/real response 与 source transaction 清理 | `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` |
| async-load capability 与 Cacheable/NC completion | `vendor/qemu_8.2.0_ub/hw/ub/ub_async_load_device.c` |
| EL1 runnable-yield | `guest-linux/aarch64/driver/linqu_ub_drv.c` |
| guest acceptance | `guest-linux/aarch64/apps/obmm_async_coroutine/obmm_async_coroutine.c` |
