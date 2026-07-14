# OBMM Object Store Step Latency Investigation

> 研究时间：2026-07-14 | 所属领域：W5 stream infer / OBMM hot object path | 研究对象类型：代码与运行时性能问题

## 一、结论

问题描述里有一半成立，另一半把不同实现和不同成本混在了一起。

**成立的部分**：guest W5 路径的 OBMM payload 使用只前进的 linear arena。每个 decode step 会为新的 hidden output 和累计 KV snapshot 分配新空间，旧 payload 没有 free、reuse 或 session reset。KV snapshot 自身随上下文变大，因此单个 step 写入的数据量近似线性增长，整个 session 的累计分配量近似二次增长。长时间运行最终会先遇到 arena exhaustion，逐步增大的 memcpy、checksum、`msync` 也会抬高 publish 成本。

**不成立的部分**：Rust `LingquObjectServiceStub::latest_record` 并不会在正常 W5 KV 路径中扫描越来越长的版本列表。它先用 `HashMap` 定位 key，再从该 key 的版本向量尾部反向查 committed record。W5 每一步把 step 编进 key，典型 key 是 `.../decode-step15` 或 `.../step/00000015`，新 key 的版本向量通常只有一个 committed record。正常调用只检查末尾一项。

**guest C 路径存在另一种线性查找**：`mem_service_find_record` 扫描固定的 1024 槽数组。每步新 key 占用靠后的槽，命中需要更多比较；未命中新 key 时总要检查全部 1024 槽。这个开销会随表填充而增加，但被 1024 封顶。现有 16-step 日志中 `kv_resolve_ms` 始终为 0-4 ms，没有显示趋势性增长，因此它不是当前端到端 step 变慢的主因。

**Rust simulator 另有更重的累计成本**：OBMM publish 为了事务性回滚会 clone 整个 pool，clone 包含已存 payload 的 `Vec<u8>`；成功或失败后又多次执行全 records 的 `report_checksum()`。当 storage ref 每步唯一时，这两项会随已发布对象和 payload 总量增长，远比 `latest_record` 更值得处理。

对用户可见的判断是：

- W5 stream infer 的后续 step 可能、并且现有日志中确实通常更慢。
- 不能据此认定 `latest_record` 是原因。
- guest 实跑的主要增长来自累计 KV、同步/校验和跨节点 barrier；linear arena 放大内存占用并制造最终容量故障。
- 若只把 `latest_record` 换成索引，用户几乎感受不到改善，arena 仍会耗尽，累计 KV 仍会重复写入。

## 二、调查边界

仓库里有两个名称相近、行为不同的 Object Service 实现。

| 路径 | 实现 | 主要用途 | 记录结构 | payload 结构 |
|---|---|---|---|---|
| Guest W5 runtime | C, `guest-linux/aarch64/components/mem_service/` | QEMU guest 内真实 W5 handoff、KV publish/resolve | 固定 `records[1024]` | OBMM region 内 linear arena |
| Simulator/reference path | Rust, `crates/sim-services` + `sim-uapi` | 仿真服务、snapshot、reference decode report | `HashMap<String, Vec<Record>>` | `HashMap<storage_ref, Payload>` + bump offset |

原问题同时使用了 guest C 的 `arena` 现象和 Rust 的 `latest_record` 函数名。如果不先拆开，会得到错误的优化优先级。

本次调查先完成归因，再落地 shortpath KV key schema 中移除 node 的改动；
P0-P3 的其余性能与生命周期改造仍是建议，尚未实现。证据来源包括：

- 当前工作树源码；
- Git 历史和 blame；
- 仓库内已有 8-node W5 guest 日志；
- `sim-services` 现有 256-step 与 500-step release tests；
- Rust 官方 Iterator 文档，用于确认 `rev().find()` 的短路语义。

## 三、Guest 本地 range-KV 路径的完整生命周期

### 3.1 每一步创建新逻辑对象

这里分析的是 KV 已物化到节点后的本地 record key，不是 W5 shortpath 的
逻辑对象 key。当前本地 key 包含 model、scope、node、layer range 和
`decode-stepN`。`mem_service_format_range_kv_state_key()` 明确把
`decode_step` 写入 key：

```text
kvcache/<model>/scope/<run>/node<id>/layers-<start>-<end>/decode-step<N>
```

因此 step 0 和 step 1 不是同一个 key 的 version 1、version 2，而是两个 key，各自 version 1。日志也验证了这一点：

```text
step=0 key=.../decode-step0 version=1
step=1 key=.../decode-step1 version=1
...
step=15 key=.../decode-step15 version=1
```

`decode-stepN` 使每一步形成独立对象，因此当前实现会保留逐 step 的不可变
引用，消费者也可以按精确 step resolve。`node` 对这个效果没有贡献；它只是
当前本地物化命名的一部分，不能据此推导出 shortpath KV 的逻辑身份必须包含
node。独立 step 对象的代价是 metadata record 和 payload 都随 step 增长。

shortpath KV 必须把逻辑身份和来源元数据分开：

```text
logical key = (run, step/token boundary, layer_range)
provenance metadata = (creator_node)
placement metadata = (execution_target, storage_location, generation)
```

`creator_node` 记录 KV 由哪个节点计算产生，不决定谁可以复用它。固定 W5 映射
下，creator、当前 layer owner 和 materialization target 恰好相同，但这是映射
结果，不是对象身份。shortpath resolve 应按 step/token boundary 和 layer range
匹配，再把对象物化到当前执行节点。

落地后的 shortpath KV artifact key schema 为：

```text
artifact/kv/<run>/step<N>/layers-<start>-<end>
```

Object Service key 使用该 artifact ID 和 version，不再间接包含 node：

```text
lingqu/memory/execution/<artifact-id>/v<version>
```

shortpath KV stream 仍携带 `creator_node`，用于 provenance 和诊断，但 guest
entry 选择不再比较 `creator_node == local_node`。这使 key、去重和 resolve
语义都不依赖创建节点。

### 3.2 KV 不是增量，而是累计 snapshot

`llm_infer_qwen3_range_kv_state_bytes()` 给出每 token、每 layer 的 KV 字节数。运行时 payload 还包含 header；随着生成 token 增加，日志中的 `kv_bytes` 每步增加 32,768 B：

| Step | KV bytes | 相对前一步 |
|---:|---:|---:|
| 0 | 131,232 | - |
| 1 | 164,000 | +32,768 |
| 4 | 262,304 | +32,768/step |
| 8 | 393,376 | +32,768/step |
| 12 | 524,448 | +32,768/step |
| 15 | 622,752 | +32,768/step |

发布路径为这份完整 payload 重新分配 span，然后执行 `memcpy`、region range update、`msync` 和 checksum 验证。假设第 `s` 步 payload 为 `B0 + s * d`，T 步累计写入量为：

```text
sum(B0 + s*d), s=0..T-1
= T*B0 + d*T*(T-1)/2
= O(T^2)
```

这里的二次增长来自“每步保存完整累计 KV”，不是 arena 查找。arena 只是没有丢弃旧 snapshot，使这份累计成本全部保留。

### 3.3 linear arena 的行为

`mem_service_payload_arena_alloc()` 做四件事：

1. 对 `payload_arena_next` 向上对齐；
2. 计算 `end = offset + bytes`；
3. 检查是否超过本 node region；
4. 把 `payload_arena_next` 和 high water 推到 `end`。

函数没有 free list、ring cursor、generation retire、session reset 或 compact。分配时间仍是 O(1)，空间只增不减。

KV allocator 还按 256 KiB、512 KiB、1 MiB、2 MiB tier 向上取整。除了 payload 自身增长，tier 边界会产生台阶式空间放大。16-step 日志中：

| Step 区间 | 实际 KV 大小 | KV reserved tier |
|---|---:|---:|
| 0-3 | 131-230 KiB | 256 KiB |
| 4-11 | 256-480 KiB | 512 KiB |
| 12-15 | 512-608 KiB | 1 MiB |

同一步还会分配 hidden range output，并受下一次 KV block alignment 影响。因此 nodeA 的 arena high water 从 step 0 的 1.5 MiB 增到 step 15 的 19 MiB：

| Step | Payload high water | Arena used |
|---:|---:|---:|
| 0 | 1.5 MiB | 0.5 MiB |
| 4 | 4 MiB | 3 MiB |
| 8 | 8 MiB | 7 MiB |
| 12 | 13 MiB | 12 MiB |
| 15 | 19 MiB | 18 MiB |

默认每 node region 为 512 MiB。不能用 `512 / 当前每步增量` 简单预测极限，因为 KV 会继续变大，2 MiB 以上还会占用多个 block。长 decode 的累计占用曲线高于线性。

### 3.4 metadata record 的增长与回收

guest 服务使用固定数组：

```c
struct mem_service_record records[MEM_SERVICE_MAX_RECORDS];
// MEM_SERVICE_MAX_RECORDS = 1024
```

`mem_service_find_record()` 从槽 0 扫到槽 1023。`mem_service_alloc_record()` 也从槽 0 开始找第一个空槽。

新 step key 的 publish 会经历：

1. `find_record(new_key)`，未命中，扫描 1024 槽；
2. `alloc_record()`，扫描到第一个空槽；
3. 写入 record；
4. 部分路径再次 `find_record(new_key)`，扫描到新槽；
5. resolve 时再次按 key 扫描。

表未满时，命中位置随 record 数增长。表满时，Qwen3 recycler 会寻找比 incoming step 至少早 16 步的最老可回收 runtime record，清空该槽后复用。recycler 自身也扫描 1024 槽。

这里有 metadata 回收，但没有 payload 回收。旧 record 被 `memset` 后，旧 OBMM span 仍被 arena high water 包含，无法复用。metadata table 能继续运行并不代表 data plane 容量可持续。

### 3.5 为什么日志不支持“lookup 是主要瓶颈”

同一个 16-step、8-node、Qwen3-0.6B W5 run 的 nodeA timing 如下。step 0 包含冷启动，分析趋势时应从 step 1 看：

| Step | KV resolve | Worker compute | Worker publish | Worker total | Barrier | Total with barrier |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2 ms | 158 ms | 35 ms | 387 ms | 2,263 ms | 2,657 ms |
| 4 | 2 ms | 164 ms | 41 ms | 446 ms | 3,527 ms | 3,983 ms |
| 8 | 1 ms | 159 ms | 67 ms | 521 ms | 4,460 ms | 4,986 ms |
| 12 | 1 ms | 163 ms | 94 ms | 661 ms | 6,139 ms | 6,804 ms |
| 14 | 1 ms | 162 ms | 95 ms | 692 ms | 6,988 ms | 7,687 ms |

观察结果：

- `kv_resolve_ms` 没有上升趋势，0-4 ms 内波动；
- compute window 在该 node 上约 158-175 ms，基本稳定；
- publish 从几十毫秒抬升到约 90 ms，符合更大 KV 校验/同步的方向，但不是严格单调；
- 端到端增长主要由 barrier 放大，说明最慢 node、跨节点等待或下游阶段才是用户感知延迟的主导项；
- 单个 nodeA 的日志不能单独定位 barrier 的根因，但足以排除“nodeA KV metadata lookup 从 2 ms 涨到数秒”。

## 四、Rust Object Service 的真实复杂度

### 4.1 `latest_record` 的最坏情况和常见情况

实现为：

```rust
self.records
    .get(key)?
    .iter()
    .rev()
    .find(|record| record.state == LingquObjectState::Committed)
```

Rust 官方文档说明 `Rev` 反转迭代方向，`find` 返回迭代方向上的第一个匹配项。复杂度取决于尾部连续的非 committed record 数量，而不是向量总长度：

- 尾元素 committed：1 次检查，O(1)；
- 尾部有 1 个 quarantined：2 次检查；
- 尾部连续有 k 个非 committed：O(k)；
- 整个向量没有 committed：O(version_count)。

当前 `publish_record()` 直接 append committed record。`quarantine_latest()` 只能把最后一项设为 quarantined；随后新 publish 又会 append committed。因此正常路径不会自然形成越来越长的 non-committed tail。

### 4.2 W5 KV key 使版本向量通常只有一项

Rust reference path 的 key 也包含 step：

```text
qwen3/session/<session>/kv/step/<8-digit-step>
```

placement storage ref 又由 key 派生为 `<key>/payload`。每步是新 key、新 storage ref、version 1。HashMap 的 key 数增长，但单 key 的 `Vec<Record>` 没有随 step 增长。

因此“每步新 KV 堆在同一个版本列表，下一次 `latest_record` 扫得更长”不符合当前代码。

### 4.3 真正随历史增长的 publish 路径

`LingquObmmPoolBackend::publish_payloads()` 先执行：

```rust
let mut staged = self.clone();
```

pool 的 clone 会复制：

- `payloads: HashMap<String, LingquObmmPoolPayload>`；
- 每个 payload 内的 `Vec<u8>`；
- delivered descriptors；
- queues 和 slot vectors；
- pending descriptors 与 stats。

随后只在 staged 上发布，成功才用 staged 替换原 pool。这提供了简单的 all-or-nothing 行为，但每次 publish 都可能复制此前全部 hot payload。若每步 storage ref 唯一，第 n 次 publish 的复制量与前 n-1 次累计 payload 成正比。

`publish_record()` 在结束时调用 `report_checksum()`；部分错误路径也调用。checksum 会排序所有 key，再遍历所有 version。第 n 个对象发布后要重新扫前 n 个对象，连续发布 N 个对象的 metadata checksum 工作量为 O(N^2)。

Rust record 还保留 `payload_bytes`，OBMM pool payload 另存一份 `payload: Vec<u8>`。同一 hot payload 在内存中至少有两份所有权数据；pool clone 时会出现更高的瞬时峰值。

这些是 simulator/reference path 的设计债务。现有 release tests 仍能通过：

| Test | 结果 | release 时间 |
|---|---|---:|
| 8-node decode-like handoff, 256 steps | PASS | 0.95 s |
| 8-node decode-like handoff, 500 steps | PASS | 2.75 s |

测试证明功能和当前规模下的可运行性，不证明单步延迟平坦。500-step test 使用 8 个循环 slot 和复用 storage ref，pool 中活跃 payload map 会趋于有界；真实 reference decode 的 storage ref 包含 step，增长模式更差。现有测试没有按 step 记录 wall time，也没有断言 p50/p99 lookup 或 publish latency 不随历史增长。

## 五、横向方案比较

### 5.1 当前方案：不可变 step snapshot + bump arena

优势是实现短、对象引用稳定、历史 step 可直接审计，失败时不会覆盖旧数据。它适合 2-step、8-step bring-up 和协议验证。

短板是长 stream infer 的目标与它冲突。KV 是持续变大的 session state，不是天然的不可变小对象。每步复制完整状态，让时间和空间成本都跟上下文增长；metadata recycler 只清记录，不清 payload，生命周期没有闭合。

### 5.2 稳定 KV object + 原地 append

为 `(session, layer_range)` 预留可扩展 backing，step 只写新增 token 的 K/V
slice，record version 记录 logical length、checksum/generation 和 committed
boundary。creator node 只作为 provenance metadata；当前 owner 和存储位置由
placement metadata 表达。

| 维度 | 表现 |
|---|---|
| 每步数据写入 | O(delta KV)，通常固定大小 |
| Session 总空间 | O(final KV)，不再是所有 snapshot 之和 |
| latest lookup | stable key，O(1) index |
| 历史审计 | 记录 metadata/version；不保留每步完整物理副本 |
| 并发风险 | 必须用 generation、commit length、reader ack 防止读半写状态 |

这是最符合 stream infer 目标的方案。复杂性应由系统承担：消费者继续拿 ObjectRef，不应被迫管理 buffer cursor。

### 5.3 有界 immutable ring

保留每步 snapshot，但只保留 `inflight + retry + audit window`，例如当前策略已有的 16 steps。payload 用 ring slot 或 size-class free list，round barrier/consumer ack 后 retire generation。

该方案改动比原地 append 小，仍能精确回读最近若干 step。空间从无界变为 `window * max_step_payload`，但每步依然复制完整累计 KV，publish 时间仍随上下文增长。它解决容量，不完全解决 step latency。

### 5.4 通用 allocator + GC

为 OBMM object store 增加 size-class slab/buddy allocator、引用计数、LRU/TTL 和 compaction，可以支持多模型、多对象类型与碎片回收。

这个方向适合通用 hot-object service，但若当前目标只是 W5 stream KV，它会把问题做大。KV 的生命周期由 session、step barrier 和 downstream ack 明确定义，优先利用这些领域信号，比先做通用 GC 更直接。

## 六、推荐改造顺序

### P0：先把性能归因做成稳定接口

在不改变语义前，补齐每 step 的分段指标：

- metadata lookup probes 和耗时；
- arena requested/reserved/reused/reclaimed bytes；
- KV memcpy、checksum、`msync` 各自耗时；
- publish descriptor 和 ObjectRef commit 耗时；
- barrier 按 peer 分解等待时间；
- Rust pool clone bytes/time 与 `report_checksum` time。

CLI 应能按 session/node/step 输出核心结论，默认只显示异常增长，详细 probes 按需展开。测试应断言字段存在、单位稳定、错误信息能给出下一步行动。

用户影响：把“step 变慢”从猜测变成可定位信号，避免优化一个 1 ms lookup，却留下 6 s barrier。

### P1：去掉无收益的线性和全量工作

Guest metadata：

- 增加 `key_hash -> slot` index；
- free slot 用 stack/bitmap，不再从槽 0 扫描；
- recycler 用按 step/generation 的队列定位候选；
- 固定数组仍可保留，避免 guest 引入复杂动态内存。

Rust simulator：

- Object history 显式缓存 `latest_committed_index`；
- preflight queue/pool capacity 后原地提交，失败只回滚本次 descriptor，不 clone 全 pool；
- report checksum 改为增量维护或仅在 report/export 时计算；
- record 与 pool payload 共享 `Arc<[u8]>`，或 record 只持 metadata/ref。

用户影响：控制面延迟更稳定，simulator 长跑不再因历史对象复制而失真。但这一步仍不能解决 guest arena 的最终耗尽。

### P2：把 KV 从“对象快照”改成“有生命周期的流状态”

推荐数据模型：

```text
logical key = (run_scope, session, model, layer_range)
provenance = (creator_node)
placement = (execution_target, storage_location, generation)
physical backing = growable/session-scoped OBMM KV region
version = committed token count + generation
step record = metadata-only audit entry
retire signal = consumer ack / round_done / session close
```

每步只 append 新 token slice。ObjectRef 带 generation、committed length 和 checksum；reader 先验证 generation，再只读 committed range。若需要 retry，可保留两个 generation 或 16-step metadata ring，不必保留 16 份完整 KV。

hidden handoff 更适合固定深度 ring，因为 payload 大小稳定、只需覆盖 pipeline inflight window。KV 和 hidden 不应共用同一个“所有对象都永久 bump”的生命周期策略。

用户影响：长 context 下单步 object-store 成本接近固定，session 总空间由最终 KV 决定，不再由所有历史 KV snapshot 之和决定。

### P3：容量失败必须可预测、可恢复

在 allocator 重构完成前，至少需要：

- 启动时根据 prompt、max decode steps、model geometry 估算 arena 需求；
- 超过安全阈值时拒绝启动，而不是运行中突然 `allocator=exhausted`；
- 日志给出 `required_bytes`、`available_bytes`、预计可支持 steps；
- session 结束显式 reset generation；
- 1024+ steps、并发 session、consumer lag、retry、stale ObjectRef 有测试。

用户影响：容量问题从中途失败变为启动前可行动的提示。

## 七、验收标准

不能只用“测试通过”证明问题解决。建议用以下可观测结果验收：

1. 单 session 1,024-step stress 下，metadata lookup p99 不随 record history 增长。
2. append KV 模式下，第 1 步与第 1,024 步的 object-store 控制面耗时接近，数据面耗时只与 delta KV 有关。
3. arena high water 在 session 生命周期内不超过 final KV + bounded inflight buffers；session close 后 generation 可复用。
4. consumer lag、retry 和跨节点 barrier 不会覆盖仍在读取的 payload。
5. stale generation resolve 返回明确错误，不会读到新 step 覆盖的数据。
6. Rust test 增加逐区间计时或 operation counters，证明 publish 不 clone 历史 payload，report checksum 不在每个 publish 全量重算。
7. CLI 能报告当前 session 的 live bytes、reclaimable bytes、high water、oldest retained generation 和预计剩余 steps。
8. 完整 `cargo test --workspace` 与 guest Python unittest 全部通过，8-node headless acceptance 再验证真实 QEMU 路径。

## 八、最终判断

这不是一个单点 `latest_record` bug，而是 hot stream state 被建模成 immutable object history 后出现的生命周期错配。

`latest_record` 的代码形态看起来是线性扫描，但当前 W5 key 设计让它通常只检查一项。真正持续积累的是完整 KV snapshot、OBMM high water，以及 Rust stub 的全池 clone/全量 checksum。guest record table 的线性扫描存在，也应该清理，但它目前是毫秒级控制面成本，不是秒级 step latency 的解释。

最直接的路径不是给历史列表再加一个索引后宣布完成，而是把 KV 作为 session-scoped append state：稳定 logical key、增量写入、generation commit、ack/barrier retire。metadata 保留审计，payload 按真实生命周期复用。这样同时解决单步增长、arena exhaustion 和记录表膨胀，而不是只压掉其中一个症状。

## 九、信息来源

### 仓库源码与日志

- `guest-linux/aarch64/components/mem_service/mem_service_obmm_objects.c`：linear arena 与 OBMM record publish。
- `guest-linux/aarch64/components/mem_service/mem_service_records.c`：固定表分配和线性 key lookup。
- `guest-linux/aarch64/components/mem_service/mem_service_qwen3_records.c`：16-step runtime record recycler。
- `guest-linux/aarch64/components/mem_service/mem_service_qwen3_kv_state_flow.c`：step key、KV allocation、copy、`msync`、publish/resolve。
- `guest-linux/aarch64/components/mem_service/mem_service_qwen3_runtime.c`：KV tier span 与 pool usage telemetry。
- `guest-linux/aarch64/apps/llm_infer/llm_infer.c`：每 token、每 layer KV bytes。
- `crates/sim-services/src/lib.rs`：Rust Object Service records、pool clone、`latest_record`、full checksum。
- `crates/sim-uapi/src/lib.rs`：step-scoped KV key 与 unique payload storage ref。
- `guest-linux/aarch64/logs/2026-05-21_19-13-09_w5_qwen3_0_6b_engram_decode_9598_headless8/nodeA_guest.log`：16-step runtime evidence。
- Git commits `ca5f57b`, `ee70e2e`, `3f19243`, `ceb6162` 及相关 blame，访问时间 2026-07-14。

### 外部一手资料

- Rust 官方 `Rev` 文档：https://doc.rust-lang.org/stable/std/iter/struct.Rev.html ，访问时间 2026-07-14。
- Rust 官方 `Vec` 文档：https://doc.rust-lang.org/stable/std/vec/struct.Vec.html ，访问时间 2026-07-14。

## 十、方法说明

报告按横纵分析法适配代码调查：纵轴追踪一个 step 从 KV 形成、arena 分配、record publish、resolve 到 retire 缺失的完整生命周期；横轴对比 guest C、Rust simulator、不可变 ring、原地 append 和通用 GC 的成本与适用边界。判断以当前源码和实跑日志为主，外部资料只用于确认标准库迭代语义。
