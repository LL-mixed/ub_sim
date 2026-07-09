# mem_service 模型适配层与 DeepSeek V4 Flash 接入方案

更新时间：2026-07-09

## 0. 文档目的与状态

本文是**review 后修订版设计方案**，不是已完成工作的记录。目标是把 `mem_service` 持续打磨成可支持多模型（不再绑定 Qwen3）的层流水线推理服务，并以 DeepSeek V4 Flash 作为第二个接入的模型，验证适配层设计。

文档组织：
- 第 1 节：目标与设计原则。
- 第 2 节：现状盘点（事实依据，标注 file:line）。
- 第 3 节：四个核心设计判断的论证（先对齐方向，再谈实现）。
- 第 4 节：分层方案与改动清单。
- 第 5 节：分阶段落地计划（阶段 0/1/2）。
- 第 6 节：测试与验收。
- 第 7 节：风险、未决项、需确认决策。

**请重点审核第 3 节（方向）和第 4.2 节（接口形状），第 5 节的阶段拆分粒度可后续打磨。**

---

## 1. 目标与设计原则

### 1.1 目标

1. **解耦**：把 Qwen3 从 `mem_service` core 中解耦为第一个"模型适配器"（adapter），mem_service core 不再依赖任何 Qwen3 符号。
2. **接入 Flash**：以 DeepSeek V4 Flash 为第二个适配器，验证适配层设计能容纳与 Qwen3 差异很大的模型（MoE、压缩注意力、专家权重按需取用）。
3. **保留既有能力**：8 节点层流水线 streaming infer（多步 decode 循环）、decode-round barrier、range handoff、KV state 对象流、对象回收等机制**行为不变**，Flash 复用同一套跨层接口。
4. **为后续打地基**：适配层建成后，后续模型接入应是"调用方新增模型几何 helper / model adapter，然后把 range-flow request 交给 mem_service"，而非让 mem_service 选择模型。

### 1.2 设计原则（与仓库现有架构法令对齐）

援引 `components/mem_service/README.md:665` 与 `:673`：

> "New model families must be added as adapters rather than renaming or specializing the service core."
> "Do not add new W4/W5-named public APIs to mem_service; W5 is a workload family, not the service boundary."

由此约束：
- **core 冻结**：`mem_service.h`（332 行，已 100% 模型无关）不新增模型相关 API。
- **适配器私有**：`mem_service_qwen3_*` 这批文件降级为 qwen3 adapter 的私有实现，core 不再直接 include。
- **权重对象化，不抢占权重真源**：`mem_service` 管理运行时对象身份、placement、生命周期、缓存账本和可审计访问；模型权重通过 model weight provider 进入系统，可被物化为 weight-tile 对象，但不要求 `mem_service` 成为所有权重的唯一真源。

### 1.3 参考实现

`/Volumes/repos/ds4`（DwarfStar）作为 DeepSeek V4 Flash 的**算法参考与实测基准**，不是可链接的库：
- `ds4.c:177-212` `DS4_SHAPE_FLASH` 是 Flash 几何的权威来源。
- `ds4_distributed.c`（层流水线 PP、TCP 环）的分布式范式与 ub_sim 的层切片一致，可互校延迟模型。
- `ds4_ssd.c` + `ds4_streaming_hotlist.inc` 的专家缓存语义是 ub_sim 建模专家按需取用的参考。
- ds4 是纯 host 侧、无 guest/驱动组件；其算法逻辑需移植进 ub_sim 的 guest C 侧与 host Rust 侧，不能直接复用二进制。

---

## 2. 现状盘点

### 2.1 mem_service 的抽象边界已经存在（关键有利条件）

- **`mem_service.h`（332 行）100% 模型无关**：OBMM 对象池、记录/审计、prefix-KV 状态机、handoff owner 投影均无 Qwen3 字样。
- **几何不是硬编码在 mem_service 内**：`mem_service_qwen3.c` 是纯透传 shim，调用 `llm_infer_qwen3_*`；真正的几何常量在 `components/llm_infer/llm_infer.c:7-18`，全部 env 可覆盖。
- **层切片是通用整除分配**：`llm_infer.c:140-169` `llm_infer_qwen3_layer_range_for_node()`，公式 `base = layers/nodes; rem = layers%nodes`，与具体模型无关。
- **对象存储已预留权重 tile 抽象**：`mem_service_object_contract.h:45` `MEM_SERVICE_OBMM_KIND_WEIGHT_TILE = 1`，Qwen3 是 dense 模型未触发，Flash 专家权重可经 provider 物化到这里。
- **多步 streaming infer 链路已验证**：guest `apps/llm_infer/llm_infer.c:11046` 的 `goto decode_round_start` 循环 + 跨节点 barrier，已验证 64 步（`sim-uapi/src/lib.rs:32843-32855`）。

### 2.2 耦合点清单（精确位置）

**Qwen3 命名文件**（15 个）：`mem_service_qwen3.{c,h}`、`mem_service_qwen3_decode_barrier.c`、`mem_service_qwen3_{engram_publish,engram_wait,kv_state,runtime_range_publish,runtime_range_wait,terminal_token}_flow.c`、`mem_service_qwen3_placement.h`、`mem_service_qwen3_record_policy.h`、`mem_service_qwen3_records.{c,h}`、`mem_service_qwen3_runtime.{c,h}`。

**名字中性但 body 内泄漏 Qwen3 的"漏文件"**（7 个，是解耦的真正难点）：

| 文件 | 泄漏点 |
|---|---|
| `mem_service_internal.h:35-37` | 聚合 include 三个 qwen3 头 |
| `mem_service_module.c:11-12` | include qwen3_records.h + qwen3_runtime.h |
| `mem_service_cluster_runtime.c:5,544` | include qwen3_runtime；用 `MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET` |
| `mem_service_cluster_queue.c:63,73-84` | 调 `mem_service_qwen3_handoff_hidden_bytes()`；定义 qwen3 token-result 描述符匹配 |
| `mem_service_cluster_queue.h:10-33` | 声明 qwen3 描述符匹配 helper |
| `mem_service_cluster_observe.c:69` | `!= mem_service_qwen3_range_nodes()` 守卫 |
| `mem_service_obmm_objects.c:45-48` | `case QWEN3_*: return "qwen3_*"` 名字串 |

**重复 13+ 次的魔数守卫** `cluster_node_count != mem_service_qwen3_range_nodes()`（魔数 8），散落在 decode_barrier.c:22,88 / kv_state_flow.c:349,489 / range_wait_flow.c:245,1024 / range_publish_flow.c:254 / engram_publish_flow.c:76,209 / terminal_token_flow.c:71 / cluster_observe.c:69 / obmm_object_flow.c:113。**全部汇聚到 `mem_service_qwen3_range_nodes()` → `llm_infer_qwen3_pipeline_nodes()`（默认 8，env 可覆盖）这一个点。**

**硬编码 key 前缀** `"qwen3/session/..."`：`mem_service_qwen3_runtime.c:217-265`。

**OBMM layout 宏**（Qwen3 固定布局）：`mem_service_object_contract.h:10-44`，含 KV state 槽位、tier 字节数、round-done 槽位、engram 槽位等。

阶段 0 对 `mem_service_object_contract.h` 的归类：
- 它不是 adapter，但也不是本阶段要清空 `qwen3` 字样的 core 源文件；它是**layout/object contract 头文件**。
- 阶段 0 不参数化 layout，因此允许它临时保留 `MEM_SERVICE_OBMM_QWEN3_*` 兼容宏。
- core `.c/.h` 文件不得继续直接使用 `MEM_SERVICE_OBMM_QWEN3_*`；应先在 `mem_service_object_contract.h` 内补模型中性别名（例如 `MEM_SERVICE_OBMM_DYNAMIC_ARENA_OFFSET`，值保持不变），再把 core 引用迁到中性名。
- layout contract 的 Qwen3 兼容宏在阶段 1 前置的 layout split/parameterization 中再清理。

### 2.3 Rust 侧的抽象缺口

- **`SIM_UAPI_W5_PROFILE` 在 Rust 全 crate 零引用**：profile 选择纯靠 shell `case` 语句散布在 11 个脚本里（`run_llm_infer_eight_node_guest.sh:12-32` 等）。**Rust 侧没有 profile registry，这是最大的抽象缺口。**
- Rust 端唯一的 profile 形状是单函数 `qwen3_dense_reference_object_service_profile()`（`sim-uapi/src/lib.rs:7455-7461`），写死 queue_depth / pool_bytes，无分派。
- 模型几何权威在 `sim-models`：`qwen3_dense.rs:63-79` `qwen3_dense_reference_profile()`（28 层 / hidden1024 / 8 kv heads / tp_nodes=8）与 `qwen3_dense_reference.rs:58-71` 的 const 双生体。
- decode loop 在 `sim-uapi/src/lib.rs:7275`（step0=prefill，step>0=decode），层→节点公式在 `:11023`（`layer_id * 8 / 28`）。

### 2.4 当前 W5 基线状态（背景，非本方案内容）

64-step W5 stream infer 的 Object Service 并发写入、snapshot profile 升级和按 decode-steps 缩放的 artifact gate 已提交为 `9836ea1 Fix W5 64-step stream infer object store handling`。验证 run id 为 `2026-07-09_14-26-24_w5_qwen3_0_6b_decode_13774`：

- `decode_steps_expected=64 decode_steps_observed=64`
- `passed_nodes=8/8`
- `worker_timing_records=512`
- `w5_run_report: status=pass`
- `PASS: eight-node w5 inference cluster`

这是本方案阶段 1 多步运行的可用基线。

---

## 3. 四个核心设计判断的论证

> 以下四点与第 3 轮讨论对齐，先固化方向，再谈实现。

### 3.1 解耦层 / adapter（方向：对）

适配层的落地形状不是在 `mem_service` 内新增 active model selector。正确边界是：`mem_service` 暴露模型无关的 range-flow request contract；`llm_infer` / 模型 runtime 作为 client 选择 Qwen3 或 Flash 几何 helper，构造 request 后传给 `mem_service`。

适配层不是全新发明——它把**已存在但隐式**的边界显式化：
- `mem_service_qwen3.h:17-33` 的 8 个几何查询函数本就是一组接口，只是现在叫 `qwen3`。
- 13+ 处 `!= range_nodes()` 守卫本就是"节点数契约检查"，只是现在用魔数。
- key 前缀本就是"命名空间隔离"，只是现在硬编码。

**约束：阶段 0 只做显式化 + 行为不变重构，不改任何机制、不引入 Flash 语义。** 这样 Qwen3 的回归可用二分法严格验证。

### 3.2 MoE 是层内概念，不冲突层流水线（方向：对）

核心洞察：**MoE 路由、专家选择、激活全部发生在单层内部**；跨层之间交出去的仍然是 hidden range。因此：

- 流水线 handoff 接口（hidden range + KV state）**不需要改**。
- decode-round barrier、range handoff、KV state 对象流的**机制不变**。

但"层内"意味着当前每层前向建模缺三块（属"补缺失环节"，非改架构）：

1. **路由决策**：Flash 每层在 attention 后跑 indexer（64 头 top-k 选 512 行）+ sinkhorn（20 iter）→ 得到本 token 激活的 6 个专家 id。该结果**层内产生、层内消费**，不跨层，是 `range_publish_flow` 每层产出的附加记录，不是新 handoff 流。
2. **专家权重寻址**：复用已预留的 `MEM_SERVICE_OBMM_KIND_WEIGHT_TILE`（kind=1），256 个专家即 256+ 个 weight tile，按 `(layer, expert_id, quant)` 寻址。
3. **专家聚合**：6 路由 + 1 共享专家的加权求和，替代当前每层单一 MLP；改的是每层前向函数，按 profile 分派 dense-MLP vs MoE-aggregate。

**结论：这三块都是给"每层前向"加料，不动任何跨层接口。** Flash 的复杂度被封在层内。

### 3.3 权重对象化：mem_service 管对象身份和缓存账本，不强占权重真源

设计假设修正为：**mem_service 是运行时对象身份、placement、生命周期、缓存账本和访问审计的权威；模型权重的真源由 model weight provider 提供，必要时物化为 mem_service 可寻址的 weight-tile 对象。**

这个边界更符合现有架构：
- `MEM_SERVICE_OBMM_KIND_WEIGHT_TILE` + `MEM_SERVICE_OBMM_WEIGHT_OFFSET`（object_contract.h:5）已经预留权重 tile 作为可寻址对象类型；Qwen3 dense 未触发，Flash 专家权重可以填上。
- `mem_service` 负责把 `(model, layer, expert_id, quant)` 映射到对象 id / backend / placement / version / checksum，不必直接持有 81GB 权重真源。
- weight provider 可以是本地文件、ub_ssd、host fixture、ds4 trace 产物或后续远端 catalog；compute 节点看到的是统一的 weight-tile object ref。
- compute 节点每层前向时据路由决策查到 6 个专家 id，向 mem_service 解析 6 个 weight tile，再由本地/远端 backend 提供 payload。

**缓存是节点侧的优化层，不是新 handoff 流**：对应 ds4 的 LRU + mlock + hotlist，但在本架构里它是"weight provider 解析出的对象在本地缓存"，而不是把 SSD pread 逻辑塞进 core。命中/未命中/淘汰统计进入 mem_service 对象读取指标，backend 细节留在 provider/cache 层。

### 3.4 层流水线负载均衡仍然对 Flash 生效

**关键事实**：当前对象存储账本里 KV 是**按层预算的槽位**，不随 token 线性增长。`llm_infer.c:123-138` `range_kv_state_bytes`：

```c
bytes_per_token_per_layer = kv_heads * head_dim * kv_streams * kv_elem_bytes;
return (layer_end - layer_start) * bytes_per_token_per_layer;
```

每层负载 = `常数 × 层数`。Flash 的层是奇偶交替（ratio-4 压缩 / ratio-128 重压缩），比如按层连续切分时任何连续区段（1-10、11-20…）都是 5 奇 5 偶，天然均衡，就算切分到某个 node 的层数是奇数，总体上仍然可以评估为均衡；

**将"压缩注意力非线性 KV → 破坏负载均衡"是误判**：误把 ds4 的"按 token 数算 KV 行数"真实增长，套到了 ub_sim"按层预算 KV 槽位"的对象存储模型上。ub_sim 层切片只关心层数，奇偶比例天然守恒。

**唯一要变的是常量替换**：Flash 不同层类型（滑窗 / ratio-4 / ratio-128）的 `bytes_per_token_per_layer` 系数不同，属常量替换，非结构变化。层切片公式（`base = layers/nodes; rem`）直接适用，43 层 / 8 节点 → 节点 0-2 各 6 层、节点 3-7 各 5 层。

---

## 4. 分层方案与改动清单

### 4.1 总体架构（目标态）

```
┌──────────────────────────────────────────────────────────┐
│  mem_service core（模型无关，冻结）                       │
│  对象身份 | placement | KV 槽位 | range handoff | barrier │
│  weight tile (KIND=1) ← provider 物化后的专家权重对象      │
└───────────────┬────────────────────────┬─────────────────┘
                │ model profile 接口      │
   ┌────────────┴───────────┐ ┌──────────┴─────────────────┐
   │ qwen3 adapter          │ │ deepseek v4 flash adapter  │
   │ · dense MLP per layer  │ │ · MoE 路由（层内）         │
   │ · 线性 KV               │ │ · 6+1 专家聚合（层内）     │
   │ · layer range = 28/8   │ │ · 压缩 KV（层内常量系数）  │
   │                        │ │ · 经 weight provider 取专家 │
   └────────────────────────┘ │ · layer range = 43/8       │
                              └────────────────────────────┘
   跨层接口（hidden range + KV state）两 adapter 完全相同
```

### 4.2 适配层接口形状

#### 4.2.1 C 侧（guest）— `struct mem_service_obmm_range_flow_request`

`components/mem_service/mem_service_profile.h` 当前不是模型 profile registry，而是 range-flow request contract。它只描述一次 OBMM range-flow 所需的模型几何结果；模型选择由调用方完成。

```c
/* mem_service_profile.h — 模型无关的 range-flow request contract */

struct mem_service_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

typedef int (*mem_service_layer_range_for_node_fn)(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint32_t *layer_start_out,
                                                   uint32_t *layer_end_out,
                                                   uint32_t *next_node_out);

typedef struct mem_service_record *(*mem_service_record_recycler_fn)(
    struct mem_service *svc,
    const char *incoming_key);

struct mem_service_obmm_range_flow_request {
    const char *model_key;
    uint32_t total_layers;
    uint32_t range_nodes;
    uint64_t hidden_range_bytes;
    uint64_t kv_state_bytes;
    struct mem_service_layer_range_placement local_placement;
    struct mem_service_layer_range_placement next_placement;
    bool has_predecessor;
    struct mem_service_layer_range_placement predecessor_placement;
    mem_service_record_recycler_fn recycle_runtime_record;
};

int mem_service_init_obmm_range_flow_request(
    struct mem_service_obmm_range_flow_request *req,
    const char *model_key,
    uint32_t total_layers,
    uint32_t range_nodes,
    uint64_t hidden_range_bytes,
    uint64_t kv_state_bytes,
    uint32_t local_node,
    mem_service_layer_range_for_node_fn layer_range_for_node,
    mem_service_record_recycler_fn recycle_runtime_record);
```

**改动后的调用约定**：
- `mem_service_obmm_service_v0_publish_resolve()` 接收调用方构造好的 `request`；不再读取 active model。
- `llm_infer` 侧根据 `SIM_UAPI_W4_CHIPBACKEND_PROFILE` 选择 Qwen3 / Flash helper，构造 request。
- `mem_service_cluster_queue` 的 descriptor matcher 接收 payload kind / payload len；Qwen3 flow 自己传 Qwen3 常量，core 不再有 `mem_service_take_pending_qwen3_*` helper。
- `mem_service_obmm_objects` 的 record recycling 接收显式 `recycle_runtime_record` callback；core 不再通过 active profile 找回收策略。
- `mem_service_object_contract.h` 保留 QWEN3 兼容宏，同时提供 `MEM_SERVICE_OBMM_KIND_MODEL_*` 中性别名供 core 使用。

`kv_state_bytes(layer_start, layer_end)` 刻意不带 `token_count`：阶段 0/1 要与现有 `mem_service_qwen3_range_kv_state_bytes()` / `llm_infer_qwen3_range_kv_state_bytes()` 签名保持一致，并遵守第 3.4 节的"按层预算槽位"模型。只有未来明确把 KV contract 改成按 token 行数计费时，才允许扩展签名。

#### 4.2.2 Rust 侧（host 模拟）— `trait ModelProfile`

`sim-models` 侧抽象（对标 `qwen3_dense.rs:63`）：

```rust
// crates/sim-models/src/model_profile.rs（新增，草案）
pub trait ModelProfile: Send + Sync {
    fn name(&self) -> &str;
    fn model_key(&self) -> &str;
    fn total_layers(&self) -> u32;
    fn range_nodes(&self) -> u32;
    fn hidden_range_bytes(&self) -> u64;
    fn handoff_hidden_bytes(&self, step: u64) -> u64;
    fn kv_state_bytes(&self, layer_start: u32, layer_end: u32) -> u64;
    fn is_moe(&self) -> bool { false }
    // 阶段 2 加 expert 相关方法
}

pub fn resolve_profile(name: &str) -> Box<dyn ModelProfile>;
```

- `qwen3_dense.rs` 的 `qwen3_dense_reference_profile()` 包装成 `Qwen3DenseProfile` 实现 trait。
- 新增 `deepseek_v4_flash.rs` 的 `DeepseekV4FlashProfile`（阶段 1 几何，阶段 2 MoE）。
- `sim-uapi/src/lib.rs:7455` `qwen3_dense_reference_object_service_profile()` 泛化成 `object_service_profile_for(profile)`。
- `sim-cli/src/main.rs` 的 `qwen3-decode-loop` 加 `--profile deepseek-v4-flash`，但不新增第三套环境变量；CLI/Rust decode entry 负责选择 Flash geometry smoke。
- decode loop `lib.rs:7275` 按 profile 分派前向（dense-MLP vs MoE-aggregate）。
- Rust 侧 `kv_state_bytes(layer_start, layer_end)` 同样不带 token count，保持与 C 侧和现有按层预算模型一致。

### 4.3 改动清单（按文件）

#### 阶段 0（解耦，行为不变）

| 文件 | 改动 |
|---|---|
| **新/改** `components/mem_service/mem_service_profile.{h,c}` | 定义模型无关 `mem_service_obmm_range_flow_request` 与初始化 helper；不保存 active model，不做 profile registry |
| `apps/llm_infer/llm_infer.c` | 在 client 侧新增 runtime helper：按 profile 选择 Qwen3/Flash geometry，构造 range-flow request 后调用 mem_service |
| `components/mem_service/mem_service_qwen3.c` | 保留 Qwen3 geometry helper，新增 `mem_service_qwen3_init_obmm_range_flow_request()`；Qwen3 adapter 作为 client request builder |
| `mem_service_internal.h:35-37` | 去掉直接 include qwen3 头，改 include `mem_service_profile.h` |
| `mem_service_module.c:11-12` | qwen3 头移到 qwen3 adapter 私有 |
| `mem_service_object_contract.h` | 保留 QWEN3 layout 兼容宏；新增模型中性 layout 别名（值不变），供 core 使用 |
| `mem_service_cluster_runtime.c:5,544` | 去掉 qwen3 runtime 头依赖；`MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET` → `MEM_SERVICE_OBMM_DYNAMIC_ARENA_OFFSET` |
| `mem_service_cluster_queue.c/.h` | `mem_service_take_pending_qwen3_*` → 模型无关 matcher，payload kind/len 由调用方传入 |
| `mem_service_cluster_observe.c` | 去掉 active model node-count guard，只保留 cluster runtime 基础合法性检查 |
| `mem_service_obmm_object_flow.c/.h` | `publish_resolve()` 接收 `mem_service_obmm_range_flow_request`，使用 request 中的 local/next/predecessor placement |
| `mem_service_obmm_objects.c/.h` | `mem_service_put_obmm_object_record()` 接收显式 recycler callback |
| `crates/sim-workloads` | 不改；当前不包含 decode/profile 分派逻辑 |

**阶段 0 范围**：仅解耦 guest C 侧 mem_service；Rust 侧 `sim-uapi`/`sim-models` 的 profile 抽象与 decode loop 解耦随阶段 1 一并进行，阶段 0 不动 Rust decode loop。

**阶段 0 约束**：所有数值断言（hidden bytes、layer range、KV bytes、对象池大小、回收策略）必须与改动前完全一致。`cargo test --workspace` + `python3 -m unittest discover guest-linux/aarch64/tests` 全绿且零数值漂移。

阶段 0 的 `qwen3` grep 门禁采用显式范围，避免和暂存的 layout contract 兼容宏冲突：
- 必须无 `qwen3` 的 core 文件：`mem_service_internal.h`、`mem_service_module.c`、`mem_service_cluster_runtime.c`、`mem_service_cluster_queue.{c,h}`、`mem_service_cluster_observe.c`、`mem_service_obmm_objects.c`，以及本阶段实际触达的其它非 adapter `mem_service_*.{c,h}`。
- 允许保留 `qwen3` 的文件：`mem_service_qwen3_*` adapter、`mem_service_object_contract.h` 的兼容 layout 宏、tests/fixtures/docs。
- core 若需要 OBMM layout 常量，必须使用 `MEM_SERVICE_OBMM_*` 中性别名，不得直接引用 `MEM_SERVICE_OBMM_QWEN3_*`。

#### 阶段 1（Flash 几何 + 几何适配）

| 文件 | 改动 |
|---|---|
| **新** `components/mem_service/mem_service_deepseek_v4_flash.{c,h}` | Flash client-side geometry helper：43 层 / hidden4096 / range_nodes=8 / 压缩 KV 系数；提供 `mem_service_deepseek_v4_flash_init_obmm_range_flow_request()` |
| `components/mem_service/mem_service_profile.c` | 不注册 flash；仅保留 neutral request 初始化 |
| `apps/llm_infer/llm_infer.c` | 识别 `deepseek-v4-flash` / `deepseek_v4_flash`，由入口选择 Flash geometry helper |
| **新** `crates/sim-models/src/deepseek_v4_flash.rs` | Flash geometry：43 层 base/rem 切分、KV bytes helper、MoE 元数据 |
| `crates/sim-models/src/lib.rs` | 导出 flash profile |
| `crates/sim-uapi/src/lib.rs` | `deepseek_v4_flash_geometry_smoke_report()` 输出 layer/handoff/barrier/KV/object 证据 |
| `crates/sim-cli/src/main.rs` | `qwen3-decode-loop --profile=deepseek-v4-flash` 打印 `flash_geometry_smoke` |

#### 阶段 2（MoE + 专家权重按需取用）

| 文件 | 改动 |
|---|---|
| `mem_service_object_contract.h` | 确认 `WEIGHT_TILE` 寻址方案（`layer, expert_id, quant`） |
| `apps/llm_infer/llm_infer.c` | 每层前向按 profile 分派：dense-MLP vs 路由+专家聚合；专家经 weight provider 解析为 weight-tile object ref |
| **新** `components/mem_service/mem_service_expert_route_flow.c` | 每层路由决策记录（层内） |
| **新** `components/mem_service/mem_service_expert_cache.c` | 节点侧 LRU + hotlist + 命中统计（参考 ds4_ssd.c） |
| `crates/sim-uapi/src/lib.rs` | decode loop 每层加 MoE 前向建模 |
| **新** `crates/sim-models/src/deepseek_v4_flash_moe.rs` | MoE 语义（路由、专家、缓存预算默认） |

---

## 5. 分阶段落地计划

### 阶段 0：Qwen3 抽象成第一个 profile（纯重构，行为不变）

**目标**：Qwen3 成为 mem_service 的第一个 adapter，core 不再 include 任何 qwen3 符号。行为零变化。

**验收**：
- `cargo test --workspace` 全绿。
- `python3 -m unittest discover guest-linux/aarch64/tests` 全绿。
- 新增"行为不变"断言：现有 `SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode` / `SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense` 映射到 qwen3 model profile 后，hidden bytes / layer range / KV bytes / 对象池大小 / 回收策略与改动前一致（可用快照对比）。
- grep 断言：明确 core 文件集合无 `qwen3` 字样；允许 `mem_service_qwen3_*` adapter、`mem_service_object_contract.h` 兼容宏、tests、fixtures、docs 保留 qwen3。

**这是安全网**。没有它，Flash 的改动会和 Qwen3 行为退化混在一起无法二分。第一刀只做纯抽象重构。

### 阶段 1：Flash geometry smoke（几何适配，不建模 MoE/缓存）

**目标**：Flash 作为第二个 client-side geometry helper 接入，跑通 8 节点层流水线的几何 smoke。这个阶段只证明 request 构造、43 层切片、hidden/KV sizing、handoff/barrier/object flow contract 正确；不宣称真实 Flash infer 能力。

**不包含**：MoE 路由、专家聚合、专家缓存（阶段 2）。每层前向先用占位（如单一等价 FFN 或仅数据搬运），重点验证几何 + 层切片 + KV 系数 + 多步循环。验收名必须叫 `flash-geometry-smoke`，不能叫 `flash-stream-infer-pass`。

**关键验证点**：
- 43 层 / 8 节点切片正确（节点 0-2 各 6 层、3-7 各 5 层）。
- Flash 压缩 KV 系数（滑窗 / ratio-4 / ratio-128 三类层）正确。
- 多步 geometry smoke 跑通（先 8 步，对标现有 macOS W5 多步运行入口）。
- 先走纯 Rust reference decode loop（`sim-cli qwen3-decode-loop` 等价路径，不启动 QEMU，避开沙箱限制）迭代；guest C 侧 + QEMU 8 节点验证留到几何稳定后。

### 阶段 2：MoE + 专家权重按需取用（Flash 性能灵魂）

**目标**：补上 Flash 区别于 Qwen3 的层内语义 + 专家缓存建模。

**三个子项**：
1. **MoE 路由建模**：每层 indexer top-k + sinkhorn → 6 专家 id。路由结果层内消费，记入 `range_publish_flow` 附加记录。
2. **专家权重按需取用**：256 专家通过 weight provider 暴露为 weight tile object ref，按 `(layer, expert_id, quant)` 寻址；compute 节点据路由查 6 个 tile。
3. **专家缓存建模**：节点侧 LRU + hotlist（借用 ds4 `ds4_streaming_hotlist.inc`）+ 命中/淘汰统计；延迟 = `max(计算时间, 缺失专家加载时间)`。缓存预算 `SIM_MODEL_EXPERT_CACHE_BYTES` / `SIM_MODEL_EXPERT_PRELOAD` 可调，扫出"缓存预算 ↔ 吞吐"曲线。

**验证**：
- 与 ds4 2 节点实测互校（prefill 1.38-1.85x、decode -19%）作为 sim 预测的 sanity check。
- 8 节点全连接 mesh 相对 ds4 软件环的 decode 改善预测（环→mesh 减少跳数）。

---

## 6. 测试与验收

### 6.1 阶段 0 测试矩阵

| 测试 | 目的 |
|---|---|
| `cargo test --workspace` | Rust 侧无回归 |
| `python3 -m unittest discover guest-linux/aarch64/tests` | guest 契约无回归 |
| 新增 `test_model_profile_qwen3_invariant` | 现有 W5/W4 profile 映射到 qwen3 model profile 后，hidden/layer/KV/池大小与改动前快照一致 |
| grep 门禁 | 显式 core 文件集合无 `qwen3` 字样；adapter、`mem_service_object_contract.h` 兼容宏、tests/fixtures/docs 允许 |

### 6.2 阶段 1/2 测试

- Flash profile 存在性、几何正确性（43 层、256 专家、压缩 KV 系数）。
- 阶段 1：8 节点 `flash-geometry-smoke` 多步通过，不宣称真实 Flash infer。
- 阶段 2：专家缓存命中率统计正确，延迟模型与 ds4 互校在合理范围。

---

## 7. 风险、未决项、需确认决策

### 7.1 已决策：阶段 0 不参数化 OBMM layout，但 core 使用中性别名

当前 OBMM 布局（`object_contract.h:10-44`）是 Qwen3 固定的，含 tier0-3 KV block 字节数等。阶段 0 的决策是：

- 不把 layout 放进 `struct mem_service_obmm_range_flow_request`；request 只承载本次 range-flow 所需的几何结果。
- `mem_service_object_contract.h` 暂时保留 `MEM_SERVICE_OBMM_QWEN3_*` 兼容宏，因此它不纳入阶段 0 的 qwen3-free core grep 集合。
- 在 `mem_service_object_contract.h` 内新增模型中性的 `MEM_SERVICE_OBMM_*` alias，值保持不变。
- 所有 core `.c/.h` 文件迁移到中性 alias；例如 `mem_service_cluster_runtime.c:544` 使用 `MEM_SERVICE_OBMM_DYNAMIC_ARENA_OFFSET`，不再直接引用 `MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET`。

这样既保持阶段 0 行为不变，也让 core grep 门禁可执行。真正的 layout split/parameterization 留到阶段 1 前置小阶段。

### 7.2 需确认：MoE 路由是真实算还是 trace 驱动

阶段 2 的 MoE 路由决策，有两个来源：
- **(A) 合成/简化路由**：sim 内用简化规则产生专家 id 序列，快速但不可信。
- **(B) ds4 实测 trace 驱动**：用 ds4 在真实权重上跑出的路由 trace 喂入，可信但依赖 ds4 跑通。

**建议 (B)**，并作为与 ds4 互校的手段。但这依赖 ds4 能跑 81GB Flash 权重，需确认硬件条件。**需确认。**

### 7.3 已完成：64-step W5 基线已提交

W5 store macOS 跨进程锁、snapshot profile 升级和 64-step artifact gate 已在 `9836ea1` 提交。阶段 0 可以直接基于该提交后的 clean tree 开始。

### 7.4 已知风险

- **decode 无法流水（Flash 更狠）**：MoE 路由依赖前一步 logits，专家缓存 miss 串行阻塞。8 节点对 Flash decode 的加速主要靠 EP（专家分散到各节点），那是指向"阶段 2 之后"的独立大项，不在本方案范围。本方案只保证 PP 正确性，decode 加速不是承诺。
- **DS4 参考的时效性**：ds4 的 Flash shape（`ds4.c:177-212`）以仓库当前版本为准，接入前需复核 ds4 是否更新。
- **guest C 侧改动量大**：`apps/llm_infer/llm_infer.c` 13K 行，阶段 2 MoE 分派改动会触及 decode 循环核心，需谨慎并充分测试。

### 7.5 本方案不包含

- 专家并行（EP）/ 张量并行（TP）——全连接 mesh 的 all-to-all 专家并行是后续独立项。
- Flash 的真实浮点计算——这是模拟器，建模数据搬运与延迟，非真实跑 81GB 权重（除非 7.2 选 trace 驱动且硬件允许）。
- ds4 二进制复用——ds4 是参考，不链接。

---

## 附：关键 file:line 索引

- mem_service core（模型无关）：`components/mem_service/mem_service.h:1-332`
- Qwen3 几何 shim：`components/mem_service/mem_service_qwen3.c:8-35`
- 几何常量源头：`components/llm_infer/llm_infer.c:7-18`
- 层切片公式：`components/llm_infer/llm_infer.c:140-169`
- KV 系数（按层预算，非线性 token）：`components/llm_infer/llm_infer.c:123-138`
- 对象存储预留权重 tile：`components/mem_service/mem_service_object_contract.h:45`（KIND=1）、`:5`（WEIGHT_OFFSET）
- decode 循环（guest）：`apps/llm_infer/llm_infer.c:11046`（round_start）、`:11294`（8节点分派）、`:13170-13216`（barrier+next）
- decode-round barrier：`components/mem_service/mem_service_qwen3_decode_barrier.c:8`（publish）、`:75`（wait-all）
- Rust decode loop：`crates/sim-uapi/src/lib.rs:7275`（循环体）、`:11023`（层→节点）、`:7455`（object service profile）
- Rust scenario 别名：`crates/sim-cli/src/main.rs:1867`
- Rust 模型几何：`crates/sim-models/src/qwen3_dense.rs:63`、`qwen3_dense_reference.rs:58`
- Flash 几何权威（参考）：`/Volumes/repos/ds4/ds4.c:177-212`（DS4_SHAPE_FLASH）
- Flash 分布式参考：`/Volumes/repos/ds4/ds4_distributed.c`
- Flash 专家缓存参考：`/Volumes/repos/ds4/ds4_ssd.c`、`ds4_streaming_hotlist.inc`
- 8 节点固定入口：`guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh`
