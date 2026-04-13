# QEMU UBus UDMA 仿真设计（对齐版）

## 1. 文档目标与范围
本设计用于指导 `simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` 上的 UDMA 仿真实施，基线对齐以下三类事实来源：

1. `ub_docs` 的架构与模块定义（作为背景约束）。
2. guest Linux 头文件/UAPI（作为接口契约）。
3. guest Linux UDMA/UBASE 驱动实现（作为行为契约）。

本版不再按“从零设计”描述，而是按“当前实现现状 + 差距修订 + 分阶段落地”组织。

## 2. 对齐基线（Spec + Driver 契约）

### 2.1 设备身份与类码
- Vendor 固定为 `0xCC08`。
- QEMU 仿真设备 ID：MUE=`0x0541`，UE=`0x0542`（`ubase_ubus.h` 与 `ubase_ubus.c` 已匹配）。
- UB 网络类码应为 `0x0002`（`ubus_ids.h` 的 `UB_CLASS_NETWORK_UB`）。

结论：`entity/device_id` 已有实现基础；类码必须与 `0x0002` 对齐。

### 2.2 CMDQ / Mailbox / CtrlQ 协议边界
- CMDQ mailbox 总 opcode：
  - `UBASE_OPC_POST_MB = 0x7000`
  - `UBASE_OPC_QUERY_MB_ST = 0x7001`
- UDMA mailbox 子 opcode（节选）：
  - JFS: `CREATE/MODIFY/QUERY/DESTROY = 0x04/0x05/0x06/0x07`
  - JFC: `0x24/0x25/0x26/0x27`
  - JFR: `0x54/0x55/0x56/0x57`
- CtrlQ 是独立通道，不等价于 mailbox：
  - service type：TP_ACL=`0x01`，DEV_REGISTER=`0x02`，QOS=`0x04`
  - 常见 opcode：`GET_TP_LIST=0x21`，`QUERY_VL=0x01`，`QUERY_SL=0x02`，`GET_SEID_INFO=0x01`

结论：设计与实现必须显式区分 mailbox 与 ctrlq 两条路径；仅在驱动 fallback 场景通过 `UBASE_OPC_UE2UE_UBASE(0xF00E)` 承载 ctrlq 消息。

### 2.3 Doorbell 与队列地址公式（硬约束）
- UAPI 常量：
  - `UDMA_JETTY_DSQE_OFFSET = 0x1000`
  - `UDMA_DOORBELL_OFFSET = 0x80`
  - `UDMA_DB_SIZE = 64`
- 驱动侧实际写 DB：`*sq->db_addr = sq->pi`。
- 驱动侧地址计算：
  - `dwqe_addr = k_db_base + JETTY_DSQE_OFFSET + UDMA_HW_PAGE_SIZE * queue->id`
  - `db_addr = dwqe_addr + UDMA_DOORBELL_OFFSET`

结论：QEMU 的 doorbell 拦截地址解析必须持续遵循该公式。

### 2.4 数据面最小语义
- WQE 基本单元：`64B`（`UDMA_JFS_WQEBB_SIZE`）。
- SGE：`struct udma_normal_sge { length, token_id, va }`。
- 当前驱动常见路径：SQ doorbell 推进 PI，设备消费至 CI，写 CQE 通知。

### 2.5 Segment/Token 语义边界
- 驱动中 `udma_segment.c` 主路径是 `ummu_sva_grant_range` + pin/unpin + ioummu map/unmap。
- 不应将 guest 驱动语义简化为“QEMU 维护 OBMM VA->GPA 映射表”这一单一模型。

结论：仿真若做 token/access 校验，应基于 WQE 字段与最小可行策略渐进增强，不能替代 guest UMMU 语义。

## 3. QEMU 当前实现盘点（As-Is）

### 3.1 已有能力
1. 实体与设备身份
- `ub_entity_table_init()` 已按 `entity0=0x0541, 其余=0x0542` 初始化。

2. CMDQ 主循环
- `ubc_process_cmdq()` 已处理 `QUERY_*`、`POST_MB`、`UE2UE_UBASE`。
- `UBASE_OPC_QUERY_MB_ST` 已返回 completed 状态。

3. Mailbox 资源管理骨架
- `ubc_handle_post_mb()` 已支持：
  - `CREATE_JFS_CONTEXT`：DMA 读 context，提取 SQ 基址/深度、TX/RX JFC、JFR 等。
  - `CREATE_JFC_CONTEXT`：提取 CQ 基址/深度。
  - `CREATE_JFR_CONTEXT`：提取 RQ 基址/深度。
  - `QUERY_JFS_CONTEXT`：回填 state/PI/CI。
  - 多个 MODIFY/DESTROY/QUERY 子 opcode 目前为 stub success。

4. Doorbell 与 SQ 消费
- `ub_ers_region_write()` 已在 ERS1 拦截 doorbell：
  - 地址模式：`addr >= 0x1080 && (addr & 0xFFF) == 0x80`
  - `jetty_id=((addr & ~0xFFF)-0x1000)>>12`
- 写入后更新 `sq_pi` 并调用 `ubc_process_sq()`。

5. 数据面收发
- `ubc_process_sq_wqe()` 可解析 64B WQE，当前主支持 `SEND`。
- 支持 inline/SGE 两种 payload 读取。
- 通过 `ub_link_write_message()` 跨节点发送。
- `ubc_handle_urma_rx_data()` 接收后写入 RQ SGE 目标地址并生成 RX CQE。

6. CtrlQ 响应
- `ubc_handle_ue2ue_ctrlq()`、`ubc_process_ctrlq()` 对 `QUERY_VL/SL`、`GET_TP_LIST`、`GET_SEID_INFO` 给出响应。
- 支持将 UE2UE 响应推回 CMDQ CRQ，兼容 guest fallback 等待机制。

### 3.2 关键不一致/未完项
1. 类码未对齐
- `UBC_CLASS_CODE` 当前为 `0x0`，应对齐网络 UB `0x0002`。

2. Mailbox 覆盖仍不完整
- 多个子 opcode 仅 stub success，缺少真实上下文字段更新与状态机。
- 个别 case 注释与 opcode 语义存在历史残留，需清理避免误导。

3. 数据面 opcode 能力不足
- 当前以 `SEND` 为主，`WRITE/READ/CAS/FAA` 未形成可验收语义。

4. CQE/中断模型仍简化
- CQE 字段、owner/phase、错误码、byte_cnt 等与驱动预期仍需细化。
- 当前中断多复用 `ubc_raise_cmdq_event()`，尚未形成明确的 CEQ/JFC 中断建模边界。

5. Segment/Token 语义未闭环
- 尚无与 token_id/access 相关的可验证执行策略（仅靠“能搬运数据”）。

## 4. 修订后的实施设计

### 4.1 设计原则
1. 以 guest 头文件与驱动行为为准，`ub_docs` 只做架构补充。
2. 先确保“驱动可用性闭环”，再增强“协议精确性”。
3. 每阶段必须有可执行脚本验收，不以日志“看起来正常”作为完成标准。

### 4.2 分阶段计划

#### Phase A（接口对齐与可维护性，P0）
- 目标：消除明显不一致，稳定当前路径。
- 修改项：
  1. 将 `UBC_CLASS_CODE` 对齐为 `0x0002`。
  2. 梳理 `ubc_handle_post_mb()` 的子 opcode 表，统一注释与实际语义。
  3. 在文档化注释中固定 mailbox/cmdq/ctrlq 的边界与调用关系。
- 验收：
  1. 双节点枚举稳定，`0x0541/0x0542` 匹配正常。
  2. UDMA 驱动探测与基础资源查询无回归。

#### Phase B（控制面闭环，P1）
- 目标：JFS/JFC/JFR 生命周期达到“可查询、可销毁、状态一致”。
- 修改项：
  1. 完整实现 `CREATE/MODIFY/QUERY/DESTROY` 的状态更新与字段回填。
  2. 规范 `QUERY_MB_ST` 与 mailbox 完成时序。
  3. 保持 CMDQ fallback 与原生 CtrlQ 两条路径均可达。
- 验收：
  1. `urma_admin`/驱动内部 query 路径稳定返回。
  2. 资源反复创建销毁后无脏状态残留。

#### Phase C（数据面增强，P1）
- 目标：在现有 SEND 基础上形成可扩展执行框架。
- 修改项：
  1. 抽象 WQE opcode dispatch（SEND/WRITE/READ/ATOMIC 分发框架）。
  2. 完善 CQE 关键字段（owner/status/byte_cnt/wqe_idx）与错误路径。
  3. 明确 RQ 消费与 CQ 生产的边界条件（空队列、越界、长度不足）。
- 验收：
  1. 单节点 loopback 稳定。
  2. 双节点 SEND/RECV 稳定且 CQ 行为一致。

#### Phase D（语义增强与鲁棒性，P2）
- 目标：补齐 token/access、异常处理、可观测性。
- 修改项：
  1. 引入最小 token/access 校验策略（不替代 guest UMMU，只做设备侧语义检查）。
  2. 完善异常 CQE/错误码与统计。
  3. 收敛调试日志，保留可开关的关键 trace 点。
- 验收：
  1. 异常注入场景可复现并返回预期错误。
  2. 长时间双节点压力下无明显状态漂移。

## 5. 验收与回归矩阵

### 5.1 必测场景
1. 设备枚举与驱动加载（含 `0x0541/0x0542`）。
2. JFS/JFC/JFR 创建-查询-销毁循环。
3. Doorbell 触发下的 SQ 消费与 CQ 生产。
4. UE2UE CtrlQ（QOS/TP/SEID）响应与 CMDQ fallback 完整性。
5. 双节点 URMA 收发与重复运行稳定性。

### 5.2 观测点
- CMDQ：opcode、bd_num、head/tail。
- Mailbox：sub_op/tag/status 与 context DMA 地址。
- Data path：jetty_id、sq_pi/sq_ci、rq_ci、cq_pi。
- Cross-node：`ub_link` 发送/接收长度与失败计数。

## 6. 本版修订结论
1. 原文中“OBMM VA->GPA 表为核心”的描述已降级为可选增强，不再作为当前实现前提。
2. 明确了 mailbox 与 ctrlq 是并行控制通道，不再混用概念。
3. 明确了当前 QEMU 已有实现基础（非从零），后续实施以“补齐差距”为主。
4. 增加了可执行分阶段验收标准，可直接指导后续开发与联调。
