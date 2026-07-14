# W5 DeepSeek V4 Flash 官方权重接入与验证计划

日期：2026-07-13

状态：下一阶段目标，尚未完成

## 1. 目标

下一阶段的目标是让 `ub_sim` 直接读取并执行 DeepSeek V4 Flash 官方
`config.json` 和 Safetensors checkpoint，在 M4 Max 128GB 平台上完成短上下文
功能正确性验证，并接入 W5 2-node、3-node、8-node streaming inference。

目标完成后的准确能力声明是：

> W5 支持官方 DeepSeek V4 Flash checkpoint 的短上下文功能验证，包括
> FP8/FP4 权重、首 token、连续 decode、MTP 和 2/3/8-node layer pipeline；
> 1M context 不在本阶段验证范围内。

本计划是
`docs/plans/2026-07-09-mem-service-model-adapter-deepseek-v4-flash.md`
之后的官方权重接入阶段。前一份计划继续保留为架构演进和历史实现记录；其关于
DS4 oracle、GGUF 权重和下一阶段验证方式的描述，以本文为准。

## 2. 硬性边界

### 2.1 DS4 只利用和参考，不修改

`/Volumes/repos/ds4` 是只读参考，不是本阶段交付物，也不是 W5 运行时依赖。

允许：

- 阅读 DS4 的模型结构、tensor mapping、FP8/FP4 解码、mHC、压缩注意力、
  router、MoE 和 MTP 实现；
- 运行现有 DS4 命令和未修改的 DS4 二进制，观察现有 GGUF 路径；
- 使用 DS4 已有工具诊断官方 Safetensors 或现有 GGUF；
- 在 `ub_sim` 中根据 DS4 和官方实现独立重写 Rust/reference 实现。

禁止：

- 修改 DS4 源码或为本计划向 DS4 增加 golden/reference 模式；
- 让 W5、`sim-models`、`sim-uapi` 或 simpler 在构建或运行时链接 DS4；
- 调用 DS4 动态库代替 W5 的模型计算；
- 把 DS4 的 IQ2/Q2/Q4/Q8 GGUF logits 当作官方 FP8/FP4 checkpoint golden；
- 把 DS4 路径通过复制、subprocess 或 fallback 隐藏在正式执行路径中。

现有 DS4 GGUF 结果只用于模型图、层数、路由形态和执行顺序等结构性对照。
由于权重格式和量化误差不同，它不能证明官方 checkpoint 的 logits 正确。

### 2.2 正式执行路径不变

官方权重的矩阵和向量计算必须经过：

```text
W5 guest
-> UAPI dispatch
-> sim-uapi
-> sim-chipbackend-simpler
-> simpler C API
-> simpler kernel
```

`sim-models` 负责模型配置、权重格式、模型语义、reference 和 lowering；
`sim-uapi` 负责把模型 operation dispatch 到 simpler；`mem_service` 只提供模型
无关的权重/KV/hidden/object 基础设施，不选择模型，也不实现模型算子。

### 2.3 官方权重必须 out-of-core

官方 checkpoint 约 160GB，不能在 128GB 内存中完整驻留。实现必须：

- 直接读取官方 Safetensors，不生成完整 BF16/F32 模型副本；
- 通过 mmap 或 positioned read 按 tensor slice 读取；
- 按 W5 node 的 layer range 访问权重；
- router 完成后只读取当前 token 选中的 routed experts；
- 对 tensor、expert 和 scratch buffer 设置明确的缓存/内存上限；
- 禁止 2/3/8-node 中的每个 QEMU backend 各自复制完整 checkpoint。

### 2.4 1M context 明确不验证

本阶段可以解析并保留：

```text
max_position_embeddings = 1048576
```

但明确不执行、不验证、不宣称支持：

- 1M-token prefill；
- 1M-token KV/cache 容量；
- 1M-token prefix cache；
- 百万位置完整数值对齐；
- 1M context 的稳定性、时延和吞吐。

任何阶段不得用短上下文测试结果外推 1M context 已可用。

## 3. 验证原则

本阶段采用两条相互独立的路径：

1. `ub_sim` 标量 CPU reference：强调清晰、确定性和数值可检查，不追求性能。
2. production simpler path：使用正式 UAPI 和 simpler C API 执行。

两条路径可以共享经过校验的 Safetensors slice reader，但不能共享 production
kernel。关键中间状态必须逐层对齐，不能只比较最终 token。

每项新能力必须同时具备：

- 稳定的命令行验证入口；
- focused unit tests；
- 负例和 fail-closed 测试；
- production/reference 对照测试；
- 可审计的模型 revision、config、index、tensor 和输入 checksum。

## 4. 分阶段实现与验收

### 4.1 阶段 1：官方模型资产 loader

实现：

- 解析官方 `config.json`，形成运行时 `DeepseekV4Config`；
- 解析 `model.safetensors.index.json`；
- 解析全部 shard header 和 tensor metadata；
- 建立 tensor name 到 shard、dtype、shape、offset 的映射；
- 关联 FP8/FP4 weight 与 scale tensor；
- 通过 mmap/positioned read 读取 tensor 或 expert slice；
- 绑定 model revision、config checksum 和 index checksum；
- 提供有界 tensor/expert cache，不使用全文件 `read_to_end`。

CLI 验证入口必须能完成：

- inspect config/index/shards；
- 校验完整 tensor schema；
- 按名称读取指定 tensor slice；
- 输出 dtype、shape、offset、payload checksum 和 scale 关联；
- 报告峰值 resident/cache bytes。

完成门槛：

- 官方 46 个 shard 全部通过 header/schema 检查；
- 任意普通 tensor 和 routed expert slice 可定位和读取；
- 缺 shard、坏 offset、坏 dtype、坏 shape、缺 scale 必须 fail-closed；
- loader 测试和完整 workspace 回归通过；
- 读取 metadata 和单 tensor slice 时内存不随 checkpoint 总大小增长。

### 4.2 阶段 2：独立 CPU reference oracle

不创建完整 BF16 checkpoint。基于官方 FP8/FP4 payload 在 `ub_sim` 中实现慢速、
确定性的标量 reference：

- FP8 E4M3 解码；
- FP4 E2M1 packed 解码；
- UE8M0 scale 解码；
- FP8 `128x128` weight block scaling；
- FP4 K 方向每 32 元素的 scale；
- BF16 rounding 和 F32 accumulation；
- dynamic activation quantization/dequantization；
- 单算子和完整单层 reference forward。

DS4 的 Safetensors、FP8/FP4 和模型图实现仅作为阅读参考，不参与 golden 生成。

CLI 验证入口必须能输入官方模型、layer、tensor、token/hidden fixture，并输出：

- 解码后的 tensor/block checksum；
- operator output；
- layer output hidden；
- selected experts 和 route weights；
- KV 状态摘要。

完成门槛：

- 固定 FP8/FP4/UE8M0 bit-pattern 测试通过；
- 官方 tensor fixture 可稳定复现；
- mHC、attention、compressor/indexer、router、shared/routed MoE 和 output
  projection 都有 reference 测试；
- golden 绑定模型和输入 checksum；
- reference 不调用 simpler production kernel，也不调用 DS4。

### 4.3 阶段 3：FP8 production 路径

实现：

- E4M3 weight 和 UE8M0 scale；
- `128x128` block-scale mapping；
- dynamic activation quantization；
- simpler C API FP8 GEMM；
- BF16/F32 输出和必要的 rounding；
- 按 tile 执行，禁止持久化整 tensor F32 副本。

覆盖范围：

- attention projections；
- grouped output projection；
- shared expert；
- output head；
- 官方 checkpoint 中其他 FP8 linear。

完成门槛：

- production simpler path 与独立 CPU reference 对齐；
- 覆盖完整 block、尾部 block、极值、零、NaN/Inf 拒绝和 scale 边界；
- shape/dtype/scale 不匹配必须 fail-closed；
- simpler native tests、Rust tests 和 workspace 回归通过。

### 4.4 阶段 4：FP4 routed expert 路径

实现：

- packed E2M1 routed expert weight；
- UE8M0 per-32-K scale；
- gate/up/down projection；
- clamped SwiGLU；
- top-6 expert combine；
- router 后按需加载 selected experts；
- expert cache 容量、淘汰和错误传播。

完成门槛：

- 单 expert、6-expert 聚合和不同 expert ID 测试通过；
- selection bias 只影响选择，route weight 使用未加 bias 的 score；
- hash-routed 和 learned-router layer 都有覆盖；
- production 与 CPU reference 对齐；
- 未选中的 expert 不得被读取；
- cache miss、eviction、坏 expert slice 和坏 scale 必须有负例；
- 完整 workspace 回归通过。

### 4.5 阶段 5：官方 checkpoint 首 token

使用官方 Safetensors 直接执行，不经过 GGUF 转换：

```text
prompt token IDs
-> embedding
-> mHC expansion
-> 43 transformer layers
-> output head
-> logits
-> top-1 token
```

CPU reference 和 production simpler path 必须使用同一个明确记录的原始 prompt
token 序列，不自动添加 BOS、chat template、thinking marker 或其他 token。

逐层证据：

- input/output hidden checksum；
- selected experts 和 route weights；
- raw/compressed KV 摘要；
- attention kind 和 compress ratio；
- tensor/expert read bytes；
- final logits checksum、top-k logits 和 top-1 token。

完成门槛：

- 43 层全部执行且无跳层、重复层或占位计算；
- production/reference 中间状态在定义的容差内逐层对齐；
- top-k logits 和 top-1 token 对齐；
- 运行使用官方 model revision 和完整 checkpoint schema gate；
- 峰值内存受控，不依赖 swap 才能避免 OOM；
- 完整 workspace 回归通过。

### 4.6 阶段 6：W5 2/3/8-node 连续推理

把阶段 5 的官方权重 production 路径接入 W5：

- 2-node、3-node、8-node layer partition；
- 每种 topology 分别运行 4-step 和 8-step；
- step 0 prefill，后续 step 单 token decode；
- hidden handoff、KV publish/restore、decode barrier 和 terminal output；
- 权重按 active topology 的 layer range 访问，不使用模型内固定 8-node 参数。

完成门槛：

- 三种 topology 使用同一 model revision 和 prompt token IDs；
- 三种 topology 的 token 序列、每层状态和 terminal logits 对齐；
- layer ownership 覆盖 `[0,43)` 且无重复、遗漏；
- 每轮 KV restore 和 handoff checksum 正确；
- 4/8-step 均无 fallback、占位 token 或 silent recompute；
- 权重、expert cache 和 QEMU resident memory 有界；
- 运行结束后无残留 QEMU；
- 完整 workspace 回归通过。

### 4.7 阶段 7：MTP layer

实现官方 `num_nextn_predict_layers = 1`：

- MTP tensor schema 和权重读取；
- embedding/hidden projection；
- MTP transformer layer；
- draft logits；
- MTP 状态与基础 decode 状态隔离；
- MTP 启用/禁用的显式运行模式。

完成门槛：

- 单步 MTP CPU reference 与 production 对齐；
- MTP 开关不改变基础模型 logits；
- draft token 和中间状态可审计；
- 2/3/8-node smoke 通过；
- 4/8-step 连续状态正确；
- 完整 workspace 回归通过。

### 4.8 阶段 8：1M context 不验证

本阶段没有 1M context 的实现或验证任务。只要求 loader 能读取并保留官方配置
值，不得因为本阶段其他验收通过而报告 1M context 已支持。

未来若单独立项，必须重新定义内存、KV/cache、位置编码、prefix cache、数据面和
长时间运行验收标准，不能复用本文的短上下文完成结论。

## 5. M4 Max 128GB 资源约束

本计划面向功能正确性，不承诺性能。开发和验收必须满足：

- 官方 checkpoint 直接存放在 SSD，建议保留至少 220--250GB 可用空间；
- 不生成完整 BF16/F32 checkpoint；
- QEMU guest、OBMM/mem_service、weight cache、scratch/KV 和 macOS 必须分别有
  可观测的内存预算；
- 测试报告记录 host RSS、cache high-water、tensor/expert read bytes 和运行时间；
- 首 token 或 4/8-step 即使耗时较长，也不能用跳过计算或降低权重精度换取通过。

M4 Max 是本阶段的正确性 test bed，不是官方 DeepSeek V4 Flash 的性能平台。

## 6. 实施顺序和提交边界

严格按阶段 1 到阶段 7 推进。每个阶段只有在 CLI、focused tests、负例、
production/reference 对照和完整回归全部通过后才能关闭并提交。

建议提交边界：

1. config/Safetensors metadata 和 slice loader；
2. CPU dtype/operator reference；
3. FP8 simpler production path；
4. FP4 routed expert production path；
5. 官方首 token 对齐；
6. W5 2/3/8-node 4/8-step；
7. MTP。

不得把尚未通过的下一阶段隐藏在同一笔完成提交中，也不得通过 fallback 把不支持
的 dtype、tensor、layer、expert 或 topology 转入旧 GGUF/DS4/synthetic 路径。

## 7. 最终 Definition of Done

以下条件全部满足，下一阶段目标才算完成：

- 官方 `config.json` 和完整 Safetensors schema 直接加载成功；
- 官方 FP8 E4M3、UE8M0、dynamic activation 和 FP4 E2M1 权重真实执行；
- 独立 CPU reference 与 production simpler path 完成单算子、单层和全模型对齐；
- 官方 checkpoint 首 token 的 43 层证据、logits 和 token 对齐；
- W5 2/3/8-node 分别完成 4-step 和 8-step；
- MTP 单步和连续运行验证通过；
- 没有 DS4 构建/运行时依赖，没有 DS4 修改；
- 没有 GGUF、synthetic、checksum-derived 或 host-native adapter fallback；
- 所有新增能力有 CLI、测试和 fail-closed 负例；
- 完整 workspace 测试通过；
- 报告明确写明 1M context 未验证、未宣称支持。

## 8. 参考

- 官方 ModelScope config：
  `https://www.modelscope.cn/models/deepseek-ai/DeepSeek-V4-Flash/file/view/master/config.json?status=1`
- 官方 Hugging Face 模型：
  `https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash`
- 官方 inference reference：
  `https://huggingface.co/deepseek-ai/DeepSeek-V4-Flash/tree/main/inference`
- 只读 DS4 参考：`/Volumes/repos/ds4`
- 当前 W5 DeepSeek 架构计划：
  `docs/plans/2026-07-09-mem-service-model-adapter-deepseek-v4-flash.md`
- 当前 W5 DeepSeek 4-step 报告：
  `docs/2026-07-13-w5-deepseek-v4-flash-huawei-plain-4step-report.md`
