# FM 动态 Entity 注入方案（可执行版）

## 1. 目标与边界
目标：在双节点 UB 模拟互联中，把“只有 FE0(ICONTROLLER/MUE) 可见、FE1(UE) 不可见”改造成可稳定工作的实现，最终让 `ubase -> udma -> ipourma` 数据面可跑通。

范围：

1. 优先复用现有 QEMU/FM/guest-kernel 机制，不引入新的全局守护进程。
2. 先打通“同一 ICONTROLLER 内 FE1/UE 可枚举与可 probe”，再做运行期动态增删。
3. 仅覆盖当前 `simulator/vendor/qemu_8.2.0_ub` 与 `simulator/guest-linux/kernel_ub` 代码路径。

非目标：

1. 不在本阶段引入 ACPI/PCI 热插拔模型。
2. 不在本阶段定义新的私有 mgmt opcode（例如 `UB_MGMT_CMD_ADD_ENTITY`）。

---

## 2. 对原文档的必要修正（评审结论）
原 `sim_fm_dynamic_entity_injection_design.md` 的主要问题是“架构方向正确，但落地点与现代码不对齐”。必须修正为：

1. **不新增 FM daemon + Unix Socket 私有协议作为 P0 依赖**。
   现有 `ub_fm.c` 已是进程内 FM（拓扑快照 + reconcile + timer 刷新），可直接承载能力扩展。
2. **不新增 `UB_MGMT_CMD_*` 私有消息族**。
   现有栈已具备可用管理面消息：`UB_MSG_CODE_POOL` 下的 `UB_DEV_REG/UB_DEV_RLS`，guest 已有完整处理器。
3. **优先补齐 entity_idx>0 全链路一致性**。
   当前多实体支持是“半完成”：`entity_count` 已引入，但 cfg 视图、capability 汇报、实体注入闭环尚未完成。
4. **先做“可验证最小闭环”**：
   `UB_SIM_ENTITY_COUNT=2` 时 FE1 被枚举并触发 `ubase` probe，随后再做运行期 add/remove。

---

## 3. 当前实现基线（已存在能力）

### 3.1 QEMU 侧（已有）

1. `entity_count` 属性已存在并可由环境变量驱动：
   - `simulator/vendor/qemu_8.2.0_ub/hw/arm/virt.c:1931-1940`
   - `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h:80-82`
2. `UB_OBTAIN_ENTITY_INFO` 已读取 `entity_count`：
   - `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c:223-248`
3. 已有 pool 消息注入范式（`UB_CFG_CPL_NOTIFY`）：
   - `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c:120-171`
4. `ub_fm.c` 已具备拓扑源加载、刷新、pending retry、本地 reconcile：
   - `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c:838-1183`

### 3.2 Guest 内核侧（已有）

1. 多实体模拟开关已存在，默认开启：
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_driver.c:39-42`
2. ICONTROLLER 路径在多实体模式会走真实 `UB_OBTAIN_ENTITY_INFO`：
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/ubus_entity.c:670-681`
3. 集群模式下 FE0 可启动用于后续枚举：
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/enum.c:1499-1511`
4. `UB_MSG_CODE_POOL` 的实体注册/释放处理已经完整：
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/pool.c:293-359`
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/pool.c:390-429`

---

## 4. 关键缺口（必须补齐）

### G1. `ub_cfg_rw()` 仍是“允许 entity_idx=1 的临时放行”，没有真正 per-entity 视图
位置：`simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c:258-269`

影响：guest 读到的 FE1 配置不稳定/不完整，后续驱动匹配和资源初始化会失败。

### G2. UBC 能力字段仍硬编码为单 UE
位置：

1. `total_ue_num/ue_num`：`simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c:1445,1452`
2. `ue_cnt`：`simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c:1775`
3. `cfg0_basic->total_num_of_ue`：`simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c:3415`

影响：内核/ubase 感知到的设备能力与 entity map 不一致。

### G3. `UB_OBTAIN_ENTITY_INFO` 报文内部字段不一致风险
位置：`simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c:234-244`

问题：`entity_nums`、`mue_nums`、`plen` 的语义需要统一；当前实现容易出现“长度与 map 数不一致”。

### G4. QEMU 侧缺少 `UB_DEV_REG/UB_DEV_RLS` 注入闭环
位置：`simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c:42-49`

问题：pool handler 数组里 `UB_DEV_REG/UB_DEV_RLS` 仍为空，说明目前没有可复用的动态注入实现入口。

### G5. FE1 设备身份约束需严格遵循 guest 现有校验
约束点：

1. `ubase` 已声明仿真 ID：`SIM_URMA_MUE=0x0541`, `SIM_URMA_UE=0x0542`。
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubase/ubase_ubus.h:35-36`
2. pool `entity_reg` 要求 `guid.type == UB_TYPE_CONTROLLER`，否则拒绝。
   - `simulator/guest-linux/kernel_ub/drivers/ub/ubus/pool.c:273-276`

影响：若 FE1 的 guid/device_id 字段设计不匹配，将直接导致 `ubase` 不 probe 或 pool 注册失败。

---

## 5. 目标架构（修正版）

```text
Top-level emulator config
  -> 生成 topology + entity plan（文件）
  -> 启动每个 node 的 QEMU（带 UB_SIM_ENTITY_COUNT）

QEMU 内置 FM (ub_fm.c)
  -> 负责 topology reconcile + entity plan diff
  -> 调用 ubc_msgq 注入 UB_MSG_CODE_POOL/UB_DEV_REG|UB_DEV_RLS

Guest ubus
  -> 收到 UB_DEV_REG: ub_setup_ent -> ub_entity_add -> ub_start_ent
  -> 新 ub device 触发 ubase probe（0xCC08:0x0542 for UE）
```

核心原则：

1. **实体状态单一来源**：QEMU 维护 `entity table`，cfg 读写与消息注入都使用它。
2. **消息协议复用**：动态注入只用现有 pool 协议，不再定义新 opcode。
3. **渐进上线**：先静态多实体（启动即 2 个实体），再运行期增删。

---

## 6. 分阶段实施（可直接开工）

## M0（P0）：静态多实体闭环（先把 FE1 稳定枚举出来）

涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c`
2. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_config.c`
3. `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
4. `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`

改动要求：

1. 引入 `entity table`（至少包含 `entity_idx/device_id/guid/eid/ueid/cna/upi/ers`）。
2. `ub_cfg_rw()` 按 `entity_idx` 返回对应 cfg 视图，不再仅“放行 idx=1”。
3. `UB_OBTAIN_ENTITY_INFO` 统一字段语义：
   - `entity_nums = entity_count`
   - `mue_nums` 与 `map[]`、`plen` 保持一致
   - CQE `p_len` 与实际写入 payload 一致
4. 所有 UE 计数字段改为来自 `entity_count`，不再硬编码 1。
5. FE0/FE1 设备 ID 规则固定：
   - `entity_idx=0 -> 0x0541 (SIM_URMA_MUE)`
   - `entity_idx>0 -> 0x0542 (SIM_URMA_UE)`

验收标准：

1. `UB_SIM_ENTITY_COUNT=2` 时，guest 能看到 FE1 对应 ub 设备。
2. `ubase` 能对 FE1 触发 probe（不要求此阶段完成动态增删）。

---

## M1（P1）：运行期动态注入（ADD/REMOVE）

涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c`
2. `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`
3. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`
4. `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/hisi/ub_fm.h`
5. （可选）新增 `simulator/vendor/qemu_8.2.0_ub/include/hw/ub/ub_pool_msg.h`

改动要求：

1. 新增注入接口：
   - `ub_inject_entity_reg(BusControllerState *s, const UBEntityDesc *e, Error **errp)`
   - `ub_inject_entity_rls(BusControllerState *s, uint32_t eid, uint8_t reason, Error **errp)`
2. 报文严格按 guest `pool.h` 布局填充（`entity_base_info` / `entity_rs_info` / `entity_rls_msg_pld`）。
3. `ub_fm.c` 增加 entity plan 源（文件）与 diff apply：
   - desired `present` 且 current `absent` -> 注入 `UB_DEV_REG`
   - desired `absent` 且 current `present` -> 注入 `UB_DEV_RLS`
4. 注入前置条件：
   - `msgq.rq_inited && msgq.cq_inited`
   - 控制器处于 cluster 模式（guest 才接受 pool 消息）
5. 失败处理：
   - 注入失败写日志并标记 entity 状态 `error`
   - 下一轮 refresh 可重试（幂等）

验收标准：

1. 运行中把 entity plan 从 1 个 UE 改为 2 个 UE，guest 新增 FE2 成功。
2. 把某 UE 从 plan 删除后，guest 能执行对应 release 流程并移除实体。

---

## M2（P1）：调试与可观测性收敛

涉及文件：

1. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c`
2. `simulator/vendor/qemu_8.2.0_ub/hw/ub/hisi/ub_fm.c`
3. `simulator/guest-linux/aarch64/scripts/run_ub_dual_node_ubcore_urma_e2e.sh`

改动要求：

1. QEMU 日志统一关键字：`entity_reg inject`, `entity_rls inject`, `entity_state`。
2. 失败时打印最小诊断：`entity_idx/eid/device_id/rsp_status/last_error`。
3. 脚本在建链 ready 后增加“实体就绪快速检查”，失败快速退出（不等长超时）。

---

## 7. 配置约定（建议）

## 7.1 启动配置

1. `UB_SIM_ENTITY_COUNT=2`：启用双实体基础能力。
2. `UB_FM_TOPOLOGY_FILE=...`：沿用现有拓扑快照机制。
3. 内核模块参数保持 `ub_sim_multi_entity=1`（默认已是 1）。

## 7.2 运行期 entity plan（建议 INI）

```ini
[entity 0]
state=present
entity_idx=0
device_id=0x0541

[entity 1]
state=present
entity_idx=1
device_id=0x0542
eid=0x10001
ueid=0x10001
cna=0x000201
upi=0x1
guid_vendor=0xcc08
guid_device=0x0542
guid_type=0x2
guid_seq=0x2
```

说明：`entity_idx=0` 由启动路径创建；运行期主要对 `entity_idx>0` 做 add/remove。

---

## 8. 回归与验收命令

1. 静态枚举回归：
   - `AARCH64_LINUX_CC=... zsh simulator/guest-linux/aarch64/scripts/run_ub_dual_node_probe.sh`
2. 业务链路回归：
   - `ITERATIONS=1 RUN_SECS=180 START_GAP_SECS=1 simulator/guest-linux/aarch64/scripts/run_ub_dual_node_ubcore_urma_e2e.sh`
3. 动态注入回归：
   - 启动后修改 entity plan（+1 UE / -1 UE），确认 guest 日志出现 `pool_rx` 对应 `UB_DEV_REG/UB_DEV_RLS` 处理。

通过门槛：

1. 双节点场景下 `linkup=1` 且 FE1 可见。
2. `ubase` 对 UE（0x0542）稳定 probe。
3. URMA e2e 用例可连续通过（先 10 轮，再扩到 50 轮）。

---

## 9. 风险与回滚开关
风险：

1. entity cfg 视图与 pool 注入字段不一致，会导致“可枚举但不可用”。
2. 运行期增删若缺少幂等保护，可能出现重复实体或幽灵实体。

回滚开关：

1. `UB_SIM_ENTITY_COUNT=1`：回到单实体。
2. `UB_FM_ENABLE_ENTITY_DYNAMIC=0`：关闭运行期 add/remove，仅保留静态模式。
3. `ub_sim_multi_entity=0`：回退 guest 侧单实体路径。

---

## 10. Definition of Done
以下同时满足才算完成：

1. M0 合入后，`UB_SIM_ENTITY_COUNT=2` 时 FE1/UE 枚举与 `ubase` probe 稳定。
2. M1 合入后，运行期 `UB_DEV_REG/UB_DEV_RLS` 注入闭环稳定可复现。
3. 双节点 URMA e2e 在回归脚本中连续通过，且失败能快速定位到“建链/枚举/驱动匹配/数据面”层级。
