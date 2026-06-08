# GVA-GSVA Architecture and Execution Plan

## 1) 目标定义（最终架构）

本设计的目标是：  
在 `ub_sim` 中实现默认主路径为 **ARM MMU + GSVA**，并提供 **GSVA-specific coherence** 的一致性闭环，而不是在关键路径上依赖 `SIM_DEC` 或 `SIM_GVA_TCG` 的替代路径。

实现规格见 `docs/sim_gva_gsva_implementation_spec.md`。本文件定义最终架构和阶段目标；implementation spec 定义协议、代码落点、状态机、迁移开关和验收命令。

最终架构目标分为三件事：

1. 映射语义由 ARM MMU 主路径驱动，`GVA/SIM_GVA_TCG` 为兼容/回退面。  
2. GSVA 地址语义贯穿整个系统：`user_va == uba == home_va` 在 GSVA apertures 内分配与共享。  
3. 一致性按 GSVA 语义主键而不是 PA 物理块主键，支持 segment 生命周期与权限变更的原子化失效/回收。

---

## 2) 术语与边界

- **ARM MMU 主路径**：真实页面表/异常路径、TLB、权限检查成为标准翻译与一致性触发入口。  
- **SIM_DEC 后备路径**：保留现有行为用于回放、兼容或功能兜底，不作为默认默认生产路径。  
- **SIM_GVA_TCG**：目前作为验证与过渡路径存在。  
- **GSVA-specific coherence**：一致性状态以 GSVA 语义对象为核，而非纯 PA 粒度。  
- **GSVA 语义键（建议）**：`{segment_id, home_va, vmid, asid, pte_offset, p_tag, cache_policy}`。

---

## 3) 设计原则

1. **先正确后最优**：先把主路径语义闭环到可验证正确，再做优化。  
2. **GSVA 锁定分配边界**：GSVA 地址必须来自经协调后的 reserve 区，并被 guest kernel + obmm 共同识别。  
3. **主语义先行**：任何一致性事件的判断以 GSVA key 为第一分类，不以 PA 为第一分类。  
4. **生命周期原子性**：segment retire/reuse 不允许产生“旧状态残留”可见性。  
5. **兼容优先**：保留现有 SIM_DEC / TCG 接口用于过渡验证，不改变既有脚本的可用性。

---

## 4) 现状对齐（已完成 vs 不足）

### 4.1 已经较完整

- `GSVA` 管理面基础闭环（bootstrap、segment 分配/激活/retire/reuse、`gva_manager` 与演示脚本）。  
- `guest kernel` 残留冲突保护：`MAP_GSVA` 标记、GSVA aperture 注册/查询/清理、`get_unmapped_area()` 防重叠保护。  
- `QEMU` 控制面：`SIM_DEC_OP_GVA_MAP` 已实现，控制消息链路已可验证。  
- `SIM_GVA_TCG` 验证链路已存在（write/read、unmap-fault、statistics）。  
- `OBMM directory MESI` 在多节点已稳定验证，作为一致性底座可继续复用。  

### 4.2 缺口

- 默认路径未切至 ARM MMU；尚未把 GVA/GSVA 映射作为主翻译/页表路径首要入口。  
- 一致性仍偏向 PA-MESI；缺少以 GSVA key 为主的状态机与远端失效语义。  
- segment 生命周期与一致性缺少完整事务化绑定（retire/reuse 与映射撤销/重建的回滚边界不完整）。  
- `GSVA-specific coherence` 缺失：
  - 映射冲突的语义判定（按 gsva_key）。  
  - 写前失效/共享读降级策略的 GSVA 语义实现。  
  - 与 `TLB`/权限变更的一致联动。  
- 主要接口仍有“演示化/验证化”痕迹，未完全产品化（如统一 CLI/daemon 能力、错误码与诊断口径）。

---

## 5) 架构设计（目标实现）

### 5.1 统一映射流

1. 应用调用 `mmap`/导入映射时携带 `MAP_GSVA` 及保留区语义。  
2. `obmm_import` + `obmm_shm_dev` 走统一 import 管道，最终在共享控制面形成 `gsva_route_entry`。  
3. ARM MMU 页表构建时记录 GSVA 元数据（可编码进 PTE 或辅助元数据表）。  
4. TLB fault、页表更新、unmap 与 segment 生命周期事件触发统一的 GSVA coherence 回调。

### 5.2 一致性模型（GSVA-specific）

- **GSVA key 与状态**：以 `gsva_key` 为一致性对象主键，状态字段包括 `I/S/E/M` 与可选 `Shared/Exclusive`、`owner/lease`、`epoch`。  
- **事件边界**：`map`, `map_update`, `unmap`, `segment_retire`, `segment_reuse`, `token_change`, `cache_policy_change`。  
- **核心规则**：
  - 写入前必须确保无其他合法共享写者冲突；对冲突副本发起 invalidation/revoke。  
  - 读共享请求仅在 key 匹配且版本一致时可复用。  
  - retire/reuse 触发全局 epoch 推进，旧版本副本一律视为失效。  
- **与 PA 的关系**：PA 仅承载实际数据位置；PA 的 MESI 可保留为“数据层实现细节”，但一致性判断入口改为 GSVA key。

### 5.3 生命周期原子性

- segment retire/reuse 与映射撤销形成一个**顺序点**：
  - 下发 `SEGMENT_RETIRED` 并等待已确认回收 ACK；
  - 只在全网确认后清理 key 旧状态；
  - 新 segment 复用必须生成新 generation，并拒绝旧 epoch 的新请求。  
- 所有回调路径应提供幂等重试与超时回滚路径。

### 5.4 QEMU 实现角色

- `ub_ubc`：主入口改造：优先走 ARM MMU 主路径，`SIM_DEC` 与 `SIM_GVA_TCG` 为兼容/诊断。  
- `obmm_coherence`：保留现有 PA-MESI 数据面，新增/并行接入 GSVA coherence 路由（按 gsva_key 选择语义路径）。  
- Stats：新增区分统计（PA-MESI vs GSVA-coherence）并保留统一导出接口。

### 5.5 Guest Linux 角色

- `gva_manager` 继续负责 address 管理协商，输出 `home_va` 与 segment 代管规则。  
- `obmm_shm_dev` 与 `obmm_import` 使用同一 GSVA key 校验规则，确保一致的 map/unmap 语义。  
- `ub_sim_decoder_service/backend` 保持 `SIM_DEC` 兼容，同时新路径走 ARM MMU hook。  

---

## 6) 执行计划（按阶段）

### Phase 1：协议冻结与语义统一（2-3 周）
1. 固定 GSVA key 定义与序列化字段（含版本与兼容位）。  
2. 明确 `cache_policy`、`p_tag`、`pte_offset` 与 `token` 校验规则。  
3. 把现有脚本/文档改为以这些字段为准的检查项。  

### Phase 2：ARM MMU 主路径接入（4-6 周）
1. 在故障路径（fault/translation）建立 GSVA 元数据路径。  
2. 在 TLB 入口/刷新路径绑定 gsva_key，替代对 `SIM_GVA_TCG` 的强依赖。  
3. 兼容 SIM_DEC 控制面，保证老流程可回放。

### Phase 3：GSVA-specific coherence 核心（6-8 周）
1. 在 QEMU 内引入 GSVA coherence 状态机。  
2. 增加事件处理：share/read/write/revoke/invalidate。  
3. 统一 segment lifecycle 与 coherence 的事务时序。

### Phase 4：回滚/错误恢复与并发稳健（3-4 周）
1. 增加超时、重试、幂等、重复执行保护。  
2. 明确 segment retire/reuse 的幂等回滚策略。  
3. 完善错误路径日志与诊断码。

### Phase 5：默认启用与回归封闭（持续）
1. 默认关闭模拟专用开关，走主路径。  
2. 在 2/4/8 节点规模做完整矩阵验证：映射、冲突、retire/reuse、故障恢复。  
3. 产出运行报告（含 run id、统计项、失败分析模板）。

---

## 7) 交付标准（Definition of Done）

- 默认启动即走 ARM MMU 主路径（不依赖 `SIM_GVA_TCG`）。  
- `user_va == uba == home_va` 的 GSVA 映射可在 2/4/8 节点下稳定运行。  
- segment retire/reuse 后，旧映射不会被错误复用，old epoch 请求被拒绝或判为 stale。  
- 所有共享写操作在 GSVA 语义上满足一致性：冲突检测、写前失效、无脏副本可见。  
- `docs` 中有更新后的接口与运行手册，所有关键脚本与日志输出含 run id + verdict + failure reason。  
- 仍保留 SIM_DEC 回退路径作为兼容，但不再作为默认。  

---

## 8) 风险与缓解

- **风险：ARM MMU 主路径改造面广**  
  缓解：分层接入，先在 QEMU hook 骨架层完成状态注入，再推进 fault/flush 路径。  
- **风险：GSVA 语义与 PA-MESI 重叠冲突**  
  缓解：明确优先级：GSVA key 判定优先，PA 只参与载体转发。  
- **风险：性能回退**  
  缓解：引入统计项并保留只读/只共享路径优化，必要时走 fast path。  
- **风险：生命周期 race**  
  缓解：segment 生命周期与 coherence 引入 epoch+ack 顺序点。  

---

## 9) 与现有设计文档的关系

- 与 `docs/sim_gva_simulation_design.md` 对齐：Phase A/B/C 迁移逻辑作为过渡，最终目标落在“默认 ARM MMU 主路径”。  
- 与 `docs/sim_gsva_shared_virtual_address_design.md` 对齐：保留 GSVA 地址协商与 manager 启动模型，扩展到一致性与主路径接入。  
- 与 `docs/qemu_obmm_directory_mesi_coherence_design.md` 对齐：目录 MESI 保留为下层数据面，不替代 GSVA-specific coherence。  
