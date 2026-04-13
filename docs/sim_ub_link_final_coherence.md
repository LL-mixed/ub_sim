# 双节点 UB Link 最终一致性方案（可执行版）

## 1. 目标与边界
目标：在双节点直连仿真中，把“时序竞争导致的枚举失败/链路不通”从偶发问题变成可控问题，做到：

1. 启动阶段不再出现“链路未稳就开始 guest 枚举”。
2. 运行阶段链路波动可自愈（可重枚举）。
3. 失败时 10 秒内快速退出并给出可定位诊断。

边界：

1. 仅覆盖双节点场景（nodeA/nodeB）。
2. 优先改动已有路径，不引入新守护进程。
3. 数据面验收以 URMA workload 脚本为准。

## 2. Ready Contract（统一“链路就绪”定义）
只有同时满足下列条件，链路状态才允许视为 `READY`：

1. `socket_connected=1`：本端已 `accept` 或 `connect` 成功。
2. `remote_guid_valid=1`：本端端口已写入对端 GUID（非全 0）。
3. `snapshot_reconciled=1`：FM 至少完成一次本地 reconcile 且端口邻接信息稳定。
4. `state_age_ms>=stable_window_ms`：状态保持稳定超过 200ms（防抖）。

任何单项不满足，都只能是 `PENDING`，不能 `cont` 放行 guest。

状态文件约定（每个端口）：

1. `*.status`：`state`, `socket_connected`, `remote_guid`, `reconcile_ts_ms`, `last_error`。
2. `*.connected`：仅用于快速探活，不作为最终唯一判据。

## 3. 分阶段实施计划

### M1（P0）：Boot Barrier 闭环化（先做）
涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_link.c`
2. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`
3. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`

改动要求：

1. `ub_link_apply()` 返回语义改为：
   - `0`：READY。
   - `1`：PENDING（例如未连上、remote info 未齐）。
   - `<0`：FAILED（不可恢复错误）。
2. 仅在 READY 时设置 `applied=true`、`remote_applied=true`。
3. `accept/connect` 成功后原子创建 `*.connected` 与 `*.status`；`deactivate` 时清理。
4. FM 将 `ret>0` 视为 pending，继续 timer refresh，不注册“fully applied”语义。
5. 脚本启动顺序保持 `-S + QMP`，但放行条件从“日志关键字”升级为“Ready Contract 检查通过”。

验收标准：

1. `START_GAP_SECS=0/1/3` 各跑 20 轮，链路建成后再 `cont`，无 `linkup=0` 初始失败。
2. 若建链失败，10 秒内退出并打印诊断（不是等待 `RUN_SECS` 超时）。

### M2（P1）：Kernel 重枚举自愈
涉及文件：

1. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/enum.c`
2. `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_driver.c`
3. （可选）`simulator/guest-linux/kernel_ub/drivers/ub/ubus/port.c`

改动要求：

1. 增加 `ub_rescan_work`（单线程 workqueue）与 `ub_schedule_rescan(reason)`。
2. 增加防抖：首次事件后 100ms 合并窗口内只触发一次。
3. 重枚举入口串行化（mutex）：禁止并发 `ub_enum_probe()`。
4. 定义安全流程：`ub_enum_remove()` + `ub_enum_probe()`，失败时保留错误码与最近一次 reason。
5. 触发条件：
   - 端口 `link_up` 从 0->1。
   - `remote_guid` 从 null -> non-null。
   - （可选）连续 N 次 snapshot 变更。

验收标准：

1. 人为延迟 nodeB 5 秒启动，nodeA 在首次失败后可自动恢复并完成 URMA 数据面。
2. 运行中断链再恢复（kill/restart 单端 QEMU），guest 能恢复链路并重新创建可用路径。

### M3（P1）：Fail-Fast + 诊断标准化
涉及文件：

1. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`

改动要求：

1. 将已存在的 `check_link_early_or_fail()` 接入主流程（`wait_for_fm_links_ready` 前后都执行一次）。
2. 失败输出统一表格：
   - `node/port`
   - `socket_connected`
   - `remote_guid`
   - `linkup`
   - `last_fm_reconcile_ts`
   - `last_error`
3. 失败退出码区分：
   - `11` 建链失败
   - `12` 拓扑不一致
   - `13` guest 枚举失败

验收标准：

1. 任意建链失败都在 10 秒内退出。
2. 诊断输出可直接判断是“socket层 / GUID层 / 枚举层”哪一层失败。

## 4. 代码级任务清单（可直接开工）
QEMU：

1. 在 `UBLinkState` 增加字段：`socket_connected`, `remote_guid_valid`, `reconcile_ts_ms`, `last_error`, `state`。
2. 新增函数：
   - `ub_link_update_status_file()`
   - `ub_link_mark_connected()`
   - `ub_link_mark_failed(const char *reason)`
3. 重写 `ub_link_apply()` 判断逻辑，禁止“未连接也置 applied”。

FM：

1. `ub_fm_refresh_topology()` 中把 `ret>0` 继续标记 `has_pending=true`。
2. 仅当 `runtime->state == READY` 再走后续“fully applied”路径。

脚本：

1. `wait_for_fm_links_ready()` 改为读 `*.status`（不是只匹配日志）。
2. 调用 `check_link_early_or_fail()`，并在失败时立即 `dump_link_diagnostics` + return non-zero。
3. 保留现有 `-S` + `cont_qemu` 流程，不回退。

内核：

1. 新增 `ub_schedule_rescan()` 与 `ub_rescan_worker()`。
2. 在合适位置挂接 link 状态变化触发。
3. 保证 rescan 过程中不会并发执行二次 `ub_enum_probe()`。

## 5. 回归与验收命令
基础回归（每项至少 20 次）：

1. `ITERATIONS=20 START_GAP_SECS=0 RUN_SECS=120 ./scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`
2. `ITERATIONS=20 START_GAP_SECS=3 RUN_SECS=120 ./scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`
3. `ITERATIONS=20 START_GAP_SECS=1 RUN_SECS=120 ./scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`

异常注入回归：

1. 只启动 nodeA，nodeB 延迟 5 秒后启动，验证自愈。
2. 运行中重启 nodeB，验证链路恢复与数据面恢复。

通过门槛：

1. 60/60 轮通过（基础回归）。
2. 异常注入场景通过率 >= 95%。
3. 建链失败平均退出时间 <= 10 秒。

## 6. 风险与开关
风险：

1. 内核 rescan 若无串行化可能引入重复实体或竞态。
2. 状态文件判定若不原子写入，脚本可能读到脏状态。

开关（便于回滚）：

1. `UB_LINK_READY_CONTRACT=0/1`：是否启用新 ready 判定。
2. `UB_KERNEL_RESCAN=0/1`：是否启用内核自动重枚举。
3. `UB_FAIL_FAST=0/1`：是否启用脚本快速失败。

## 7. Definition of Done
以下全部满足才可关闭该项：

1. M1/M2/M3 代码合入并默认开启。
2. 回归命令连续两天稳定通过。
3. 不再出现“guest 首次枚举时 linkup=0 且后续无自愈”的缺陷。
4. 失败日志可在 2 分钟内定位根因层级。
