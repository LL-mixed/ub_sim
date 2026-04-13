# 跨节点内存直访仿真实施设计（修订版）

## 1. 文档目标与范围
本设计用于指导 `sim_ub_cross_node_memory_access_design` 的后续实现，目标是把“跨节点内存直访”从概念方案修订为可落地方案。

本版基线同时对齐三类事实来源：

1. `ub_docs` 规范语义（UBMD、UMMU、Decoder、OBMM 流程）。
2. guest Linux 现有驱动接口与行为（`ubus/decoder`、`ubus/memory`、`obmm`）。
3. QEMU 现状（已具备的控制通道/跨节点消息能力、未具备的 decoder 数据面能力）。

本版重点回答一个核心问题：
**guest 侧需要一个“模拟 decoder 驱动”，向上服务 OBMM，向下通过管控通道下发 decoder 配置命令，打通节点间内存直接访问通路。**

说明：CFG9 在 UB Spec 中是内存访问报文格式之一，本设计仅将其作为协议背景，不将其作为方案名或主线。

---

## 2. 对齐基线（Spec + Driver + QEMU）

### 2.1 规范语义（`ub_docs`）

1. UB Base Spec 2.0 明确内存访问核心对象是 UBMD：`{EID, TokenID, UBA}`。
2. UMMU 处理流程是：`TECT(EID) -> TCT(TokenID) -> MATT(UBA->PA) + MAPT(权限)`。
3. UB Decoder 负责 `PA -> UBMD` 转换，按页表/范围表完成映射。
4. OS RD 2.0 明确：IMPORT 可把“访问凭据元组”配置到 UB Decoder，之后同步 load/store 可走 UB 访问 Home 侧内存。

结论：该设计不能只描述“自定义报文”，必须落实为 `Decoder(发起) + UMMU(目标)` 的配置和运行闭环。

### 2.2 guest Linux 现有契约

1. `ubus/decoder` 已有通用接口：
   - `ub_decoder_map()` / `ub_decoder_unmap()`
   - `struct decoder_map_info` 已包含 `pa/uba/size/token_id/token_value/eid/upi/src_eid` 等关键字段。
2. HiSilicon 实现中，decoder 命令通过 cmdq/evtq + `hi_decoder_cmd_request()` 完成（含 `TLBI_PARTIAL` + `SYNC`）。
3. `ubus/memory` 已对上提供 OBMM 依赖接口：
   - `ub_memory_validate_pa()`
   - `ub_mem_drain_start()` / `ub_mem_drain_state()`
4. OBMM 现状：
   - `obmm_export` 可产出 `tokenid + uba`。
   - `obmm_import` 已存在 sim decoder callback 挂点。
   - `obmm_import` 已能从 `region->priv` 解析 `remote_uba/token_value`。
   - 但当前仍缺少实际 callback 注册者和稳定的控制通道实现，因此尚未形成真实 decoder 配置调用链。

结论：guest 并非“没有 decoder 能力”，也不是 `obmm` 完全没有挂点；真正缺的是“实际注册 callback 并驱动 QEMU backend 的 sim decoder 服务实现”。

### 2.3 QEMU 现状约束

1. `ub_ubc.c` 已暴露 CFG1 decoder capability（含 cmdq/evtq/cfg/wmask）。
2. QEMU 中已存在 `SIM_DEC_OP_MAP/UNMAP/SYNC/QUERY`、`decoder_map_table`、`sim_dec_lookup_by_pa()` 以及基于 strict DMA 路径的跨节点 WRITE/READ 数据处理。
3. `hw/ub/hisi/ubc_msgq.c` 已能通过 `SIM_DEC_CTRL_CMD` 分发私有控制命令到 `ubc_handle_sim_dec_message()`。
4. 控制面基础存在：
   - CMDQ / Mailbox / UE2UE / CtrlQ 路径可用。
   - `ub_link_write_message()` 跨节点消息发送机制可复用。

结论：QEMU backend 不再是“从零开始”，当前主缺口已经转移到 guest 侧 sim decoder service / ctrl adapter 的落地与端到端验收闭环。

### 2.4 基于代码的实现状态矩阵

1. 已存在：
   - QEMU `SIM_DEC_OP_*` 协议定义与命令处理。
   - QEMU `decoder_map_table`、overlap 检查、`MAP/UNMAP/QUERY/SYNC` 基本处理。
   - QEMU `sim_dec_lookup_by_pa()` 与基于 strict DMA 路径的远端 WRITE/READ 分发。
   - QEMU `ubc_msgq` 对 `SIM_DEC_CTRL_CMD` 的消息收发适配。
   - guest `obmm_import/unimport` 中的 sim decoder callback 挂点。
   - guest `obmm_import` 对 `region->priv` 中 `remote_uba/token_value` 的解析。

2. 部分存在但未闭环：
   - guest `obmm` 侧已有 callback registration API，但当前缺少实际注册者。
   - `obmm_sim_decoder.h` 只有结构和注册接口，缺少对应 `.c` 实现和模块初始化。
   - 设计文档中提到的 `ub_sim_decoder_service` / `ctrl_adapter` 在代码树中尚未形成稳定模块边界。

3. 仍未完成：
   - guest 到 QEMU 的稳定控制通道实现。
   - `obmm_import -> sim decoder map -> QEMU ack -> import success` 的真实联调验证。
   - cross-node memory access 的专用 e2e 用例与回归门槛。
   - READ/WRITE/UNMAP/SYNC 的故障注入与一致性验收。

---

## 3. 当前方案不足（必须修订）

旧版文档存在以下不可实施问题：

1. 把 guest 侧写成抽象“ubus 驱动配置 decoder”，没有明确独立服务层、接口、注册点和模块装配方式。
2. 没有定义 OBMM 到 decoder 的契约字段来源（尤其 `UBA/Token/EID/scna/dcna`）。
3. 没有把“向下管控通道”落到现有机制（msg/cmdq/ctrlq）上。
4. 错把 QEMU 描述成“仅 capability 暴露、backend 未实现”，和当前代码现状不符。

---

## 4. 修订后的总体架构

```text
OBMM (import/export/ownership)
  -> UB Sim Decoder Service (guest 新增服务层)
      -> Control Channel Adapter (msg/cmdq/ctrlq 适配)
          -> QEMU Decoder Emu Backend (新增)
              -> ub_link 内存访问报文传输
                  -> Peer QEMU UMMU/Memory backend
```

设计原则：

1. 不推翻现有 `ubus/decoder` 与 `ubus/memory`，通过“服务层 + 适配层”补齐仿真闭环。
2. 对上（OBMM）给稳定 API；对下（QEMU）给版本化控制协议。
3. 先打通 WRITE/READ 最小闭环，再增强 token/value、RAS、一致性。

---

## 5. guest 侧“模拟 decoder 驱动”设计

### 5.1 模块边界

新增逻辑模块（名称可调整，建议）：`drivers/ub/ubus/sim/ub_sim_decoder_*`

职责划分：

1. `ub_sim_decoder_service`
   - 面向 OBMM 的统一服务入口。
   - 维护 map 生命周期（create/update/destroy）和本地句柄。
2. `ub_sim_decoder_ctrl_adapter`
   - 把 map/unmap/sync 转为控制通道命令（请求/响应）。
3. `ub_sim_decoder_backend_select`
   - `sim backend`：走控制通道到 QEMU。
   - `hw backend`：可回退复用 `ub_decoder_map/unmap`。

### 5.2 向上（OBMM）接口契约

建议在服务层定义稳定请求结构（字段与 `decoder_map_info` 对齐）：

```c
struct ub_sim_dec_map_req {
    u64 local_pa;
    u64 size;
    u64 remote_uba;
    u32 token_id;
    u32 token_value;
    u32 scna;
    u32 dcna;
    u8  seid[16];
    u8  deid[16];
    u32 upi;
    u32 src_eid;
};
```

服务 API：

1. `int ub_sim_decoder_map(struct ub_sim_dec_map_req *req, u64 *map_id);`
2. `int ub_sim_decoder_unmap(u64 map_id);`
3. `int ub_sim_decoder_sync(u64 map_id, u64 offset, u64 len);`

OBMM 挂载点（建议）：

1. `obmm_import` 成功路径：`prepare_import_memory()` 后、`register_obmm_region()` 前执行 map，失败则回滚 import。
2. `obmm_unimport` 路径：`release_import_memory()` 前执行 unmap。
3. `ownership` 写回路径：继续复用 `ub_mem_drain_start/state`；必要时追加 `ub_sim_decoder_sync`。

### 5.3 OBMM 元数据缺口与补齐

现状事实：

1. `obmm_cmd_export` 输出有 `tokenid + uba`。
2. `obmm_cmd_import` 目前无显式 `uba` 字段（仅 `tokenid/scna/dcna/eid/...`）。

为可实施，需补齐“import 侧拿到 remote_uba”的机制，分两步：

1. P0（不改 UAPI）：在 `cmd_import->priv` 定义仿真专用结构（含 `remote_uba/token_value`），由服务层解析。
2. P1（推荐）：扩展 `obmm_cmd_import`，显式增加 `uba`（及可选 `token_value`）字段，去除隐式约定。

### 5.4 向下（管控通道）协议

定义仿真 decoder 控制命令集（版本化）：

1. `SIM_DEC_OP_MAP`
2. `SIM_DEC_OP_UNMAP`
3. `SIM_DEC_OP_SYNC`
4. `SIM_DEC_OP_QUERY`

请求/响应统一头：`version/opcode/seq/status/payload_len`。

MAP payload 至少包含：
`local_pa/size/remote_uba/token_id/token_value/scna/dcna/seid/deid`。

通道选择：

1. 主路径：`ubus message_sync_request`（现有可用，最小改动）。
2. 兼容路径：后续可接入 `ubase ctrlq`（与 QEMU 现有 ctrlq 能力对齐）。

错误语义：

1. 请求失败必须可重试（幂等 key：`local_pa + size + token_id + remote_uba`）。
2. 返回值需区分：参数错误、资源冲突、后端不可用、超时。

---

## 6. QEMU 侧配套实现要求

QEMU 侧已存在 decoder emu backend 基础，实现上应改为“补强和验收”，而不是“从零新增”：

1. 维护 `decoder_map_table`（可按 PA 区间组织，支持 overlap 检查）。
2. 接收并处理 `SIM_DEC_OP_*` 控制命令，返回明确状态码。
3. 在访问路径中命中已映射区间时，构造内存访问请求（报文格式按 UB Spec 对齐）并通过 `ub_link` 发往对端。
4. 对端执行 UMMU/权限校验，完成目标内存读写，READ 场景回包。

实现边界：

1. P0/P1 先覆盖仿真验证所需地址窗口（以当前可控 MMIO/decoder aperture 为主）。
2. 不承诺一次性覆盖所有 guest RAM/hotplug 场景。

---

## 7. 端到端时序（WRITE 示例）

1. Home 节点 `obmm_export` 产出 `{deid, uba, tokenid}`。
2. User 节点 `obmm_import` 提交 `{scna, dcna, tokenid, eid...}` 与 `remote_uba` 元数据。
3. `ub_sim_decoder_service` 生成 MAP 请求，经控制通道下发到本地 QEMU。
4. QEMU 建立 decoder 映射条目并应答成功。
5. guest 对导入地址执行 store。
6. QEMU 命中映射后构造跨节点 WRITE 请求，经 `ub_link` 发往 Home 节点。
7. Home 侧 UMMU/权限校验通过后写入目标内存。
8. 需要一致性保障时，OBMM 调用 `ub_mem_drain_*` 与 `ownership` 机制完成刷写/可见性控制。

---

## 8. 实施分期与验收

### Phase A（P0）：接口与控制面打底

修改项：

1. 新增 guest `ub_sim_decoder_service` 与 `ctrl_adapter` 实现文件，而不是只停留在头文件和文档。
2. 模块初始化时注册 `obmm_register_import_callback()` / `obmm_register_unimport_callback()`。
3. 复用现有 `obmm_import/unimport` 挂点，打通最小 `map/unmap` 请求-响应链路。
4. 统一确定控制面主路径；P0 只选一条，建议优先 `HiMsgQ HISI_PRIVATE/SIM_DEC_CTRL_CMD`，不要同时铺开多条控制面。

验收：

1. import/unimport 可触发完整请求-响应链路。
2. 异常返回可传递到 OBMM 并触发回滚。

### Phase B（P1）：MAP/UNMAP 生效与 WRITE 闭环

修改项：

1. 基于现有 QEMU `decoder_map_table` 和 strict DMA hook，完成 guest 真实接入验证。
2. WRITE 访问命中映射后可跨节点写入目标内存，并补 guest/QEMU 双侧可观测日志。
3. 完成最小 token 校验（至少检查 token_id 非空与条目匹配）。

验收：

1. 双节点写入可达、数据一致。
2. unmap 后访问失败并返回预期错误。

### Phase C（P1+）：READ/SYNC 与一致性

修改项：

1. 基于现有 QEMU READ request/response 基础，补齐 guest 侧闭环验证。
2. `SIM_DEC_OP_SYNC` 与 `ub_mem_drain_*` 协同。
3. ownership 场景回归。

验收：

1. READ/WRITE 双向稳定。
2. cacheable/non-cacheable + ownership 基本场景通过。

### Phase D（P2）：规范增强与鲁棒性

修改项：

1. 完整 token_value、权限位、错误码/RAS 上报。
2. 监控与调试可观测性（map 表、命令统计、失败注入）。
3. UAPI 显式化（import 增加 `uba/token_value`）。

验收：

1. 异常注入可复现且行为稳定。
2. 长时间双节点压力无状态漂移。

---

## 9. 关键实现约束

1. 不能把 OBMM 简化为“QEMU 内部 VA->GPA 表”；必须保留 `EID/Token/UBA` 语义链。
2. 不能跳过控制面直接改 QEMU 内部表；所有映射必须可追踪到 guest 命令。
3. 不能只做 capability 暴露；必须有 map/unmap 的执行与数据面命中。
4. import 侧 `remote_uba` 来源必须显式定义并可审计，避免隐式魔法字段。
5. 不能为了推进 cross-node memory access 破坏当前已通过的 URMA/chat/rpc/rdma 数据面；每个阶段都必须复跑 dual-node 3-demo 作为回归闸门。

---

## 10. 下一步实施清单（基于当前代码）

按当前代码基线，建议直接按以下顺序推进：

1. guest 侧新增 `ub_sim_decoder_service.c`
   - 注册 `obmm_import/unimport` callback。
   - 完成 `obmm_sim_dec_import_info <-> SIM_DEC MAP/UNMAP` 结构转换。

2. guest 侧新增控制适配层
   - 先只实现一条控制面主路径。
   - 做同步请求/响应，不做异步扩展。

3. 增加最小 e2e 验证程序
   - 构造一段 import memory。
   - 验证 cross-node WRITE 后对端可见。
   - 再补 cross-node READ。

4. 加回归门槛
   - 每次改动后先跑 cross-node memory access e2e。
   - 再跑 dual-node `chat -> rpc -> rdma`，确认不破坏既有数据面。

---

## 11. 本版结论

本版将方案进一步从“可实施的分层设计”收敛为“基于当前代码现状的实施设计”：

1. 明确 QEMU backend 与 `obmm` 挂点已有部分实现，当前主缺口在 guest 侧 service / ctrl adapter。
2. 明确 `remote_uba/token_value` 的 import 元数据路径已经存在，可直接复用。
3. 明确后续实施应以“guest callback 注册 + 控制面落地 + e2e 验证”三件事为主线。
4. 明确任何推进都必须以“不破坏当前 dual-node URMA 数据面回归”为前提。
