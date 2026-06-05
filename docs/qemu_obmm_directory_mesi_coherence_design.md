# QEMU OBMM Directory-Based MESI Coherence Design

## 1. 目标

本文档目标是在 QEMU backend 中为跨节点 OBMM shmem 实现 directory-based cache coherence，使 guest app 可以通过 normal cacheable mmap 访问从其他节点导入的 OBMM shmem，并允许内核参数继续使用：

```text
obmm.skip_cache_maintain=1
```

这个目标的本质不是跳过一致性，而是把一致性责任从 guest 显式 cache maintenance 转移到 QEMU backend。guest 侧不再依赖 `hisi_soc_cache_maintain()` 和 `ub_mem_drain()` 来保证跨节点可见性；QEMU 侧需要提供等价的 coherence、writeback、invalidation、drain 和 ordering 语义。

## 2. 非目标

1. 不模拟真实 CPU L1/L2/L3 的微架构延迟、替换策略和 speculative 行为。
2. 不要求 QEMU TCG 精确暴露每条 guest cache maintenance 指令。
3. 不把 `SIM_DEC` 继续扩展成长期架构概念；它可以作为 legacy transport/backend，新的抽象应命名为 `GVA/OBMM coherence` 或 `OBMM directory`。

## 3. 核心语义

QEMU backend 需要为 imported OBMM shmem 维护 line-granularity coherence directory。推荐先使用 64B cache line 粒度，后续可以允许 page-level coarse mode 作为性能折中，但 MESI 正确性以 line 粒度定义。

每个 cache line 的 directory entry 至少包含：

```c
enum ObmmCoherenceState {
    OBMM_COH_I,
    OBMM_COH_S,
    OBMM_COH_E,
    OBMM_COH_M,
};

struct ObmmDirectoryLine {
    uint64_t line_addr;
    uint32_t home_cna;
    uint32_t owner_cna;
    uint64_t sharer_bitmap;
    uint64_t version;
    enum ObmmCoherenceState state;
    bool dirty;
    bool pending;
};
```

语义：

1. `I`：没有节点缓存该 line。
2. `S`：一个或多个节点可读共享，无 dirty owner。
3. `E`：单个节点独占 clean copy，可以本地升级为 `M`。
4. `M`：单个节点拥有 dirty copy，shared memory backing 可能不是最新。

QEMU 的 persistent point 定义为：该 line 的最新数据已经应用到 home/export node 的 OBMM shared memory backing，且所有早于该操作的 coherence 消息已经完成。

## 4. 设计位置

推荐新增 QEMU 层模块：

```text
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.h
```

`ub_ubc.c` 保留 SIM_DEC map、UB Link read/write 和 data-path hook，但把 coherence 状态机下沉到 `obmm_coherence_*` API。

建议 QEMU 入口：

```c
int obmm_coh_map(const ObmmCohMapReq *req, uint64_t *coh_map_id);
int obmm_coh_unmap(uint64_t coh_map_id);
MemTxResult obmm_coh_cpu_read(uint64_t coh_map_id, uint64_t off, void *buf, size_t len);
MemTxResult obmm_coh_cpu_write(uint64_t coh_map_id, uint64_t off, const void *buf, size_t len);
MemTxResult obmm_coh_dma_read(uint64_t pa, void *buf, size_t len);
MemTxResult obmm_coh_dma_write(uint64_t pa, const void *buf, size_t len);
int obmm_coh_fence(uint64_t coh_map_id, uint64_t off, uint64_t len, uint32_t flags);
```

## 5. Guest/QEMU ABI

现有 `SIM_DEC_OP_GVA_MAP` 已能携带 `cache_policy`、`map_source`、`address_profile` 等 metadata。建议新增 cache policy：

```c
#define OBMM_SIM_DEC_CACHE_POLICY_NC              0
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH   1
#define OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI   2
```

当 `cache_policy == DIRECTORY_MESI` 时，QEMU 不再把该 mapping 当普通 SIM_DEC CPU window，而是注册到 OBMM coherence directory。

建议新增或扩展 control op：

```text
SIM_DEC_OP_COH_FENCE
SIM_DEC_OP_COH_QUERY
SIM_DEC_OP_COH_INVALIDATE
SIM_DEC_OP_COH_WRITEBACK
SIM_DEC_OP_COH_DRAIN
```

这些 op 不一定都暴露给用户态；它们可以先作为 QEMU/guest debug 和测试支架，用于证明状态机的 persistent point 与 ack 语义。

## 6. QEMU data path

### 6.1 CPU window read

流程：

```text
guest load imported_pa
  -> QEMU MemoryRegion read
  -> obmm_coh_cpu_read()
  -> directory lookup line
  -> if local node has S/E/M valid copy: read local coherent cache
  -> if line is I: issue GetS to home
  -> if remote owner is M: owner writeback or data-forward
  -> directory transitions to S
  -> return data
```

### 6.2 CPU window write

流程：

```text
guest store imported_pa
  -> QEMU MemoryRegion write
  -> obmm_coh_cpu_write()
  -> directory lookup line
  -> if local node has M: update local coherent cache
  -> if local node has E: E -> M, update local coherent cache
  -> otherwise issue GetM
  -> invalidate sharers and wait ack
  -> if old owner is M, force writeback or transfer ownership
  -> directory transitions to M owned by local node
  -> update local coherent cache
```

### 6.3 DMA read/write

现有 strict DMA path 已经能通过 `sim_dec_lookup_by_pa()` 命中 imported PA 并转成 remote read/write。MESI 模式下不能直接绕过 directory，否则 DMA 会破坏 CPU window coherence。

要求：

1. DMA read 命中 directory mapping 时必须走 `obmm_coh_dma_read()`。
2. DMA write 命中 directory mapping 时必须走 `obmm_coh_dma_write()`。
3. DMA write 必须获取 exclusive ownership 或 invalidate affected sharers。
4. DMA read 遇到 dirty owner 必须从 owner 获取最新数据，或先强制 owner writeback。

## 7. Coherence 消息

推荐在 UB Link SIM_DEC data-plane 之上新增 coherence sub-op，而不是复用普通 read/write 表达所有语义。

消息类型：

```text
COH_GETS          requester wants shared read permission
COH_GETM          requester wants exclusive write permission
COH_INV           invalidate cached line
COH_INV_ACK       invalidate completed
COH_WB            dirty owner writes back line
COH_WB_ACK        writeback reached persistent point
COH_DATA          data response
COH_FENCE         drain all prior ops in range
COH_FENCE_ACK     range reached persistent point
```

关键 ordering：

1. `COH_GETM` 必须等待所有 `COH_INV_ACK`。
2. dirty owner 的 `COH_WB_ACK` 必须在 directory 切换 owner 前完成，除非实现 data-forward ownership transfer。
3. `COH_FENCE_ACK` 表示 target 已处理完该 range 内所有早于 fence 的 coherence op。

## 8. Directory home 选择

建议 home node 固定为 export owner node，即 OBMM export 所在 CNA。

理由：

1. OBMM shared memory backing 位于 export node。
2. persistent point 可以定义为 export node backing。
3. bootstrap registry 已经记录 `export_cna/remote_uba/token_id`。

每个 imported map 的 QEMU entry 应保存：

```c
struct ObmmCohMap {
    uint64_t map_id;
    uint64_t local_pa;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t local_cna;
    uint32_t home_cna;
    uint32_t token_id;
    uint32_t cache_policy;
};
```

## 9. 本地 coherent cache

QEMU 需要一个 per-node local coherent cache，而不是复用现有 SIM_DEC `sync_shadow`。

建议结构：

```c
struct ObmmLocalCacheLine {
    uint64_t line_addr;
    uint8_t data[64];
    enum ObmmCoherenceState state;
    bool dirty;
    uint64_t version;
};
```

区别：

1. `sync_shadow` 是显式 sync 后的 read shadow，不维护 ownership。
2. directory MESI cache 必须维护 local line state 和 dirty owner。
3. `SIM_DEC_WRITE_MODE=write-back` 是 legacy 调试开关，不能等价为 MESI。

## 10. 与 `obmm.skip_cache_maintain=1` 的关系

启用 `DIRECTORY_MESI` 后，`obmm.skip_cache_maintain=1` 是合理的，因为 guest 显式 cache maintenance 不再是一致性的来源。

但 QEMU 必须保证：

1. guest write 在 QEMU coherent cache 中进入 `M`。
2. 其他节点 read 能通过 directory 获取最新数据。
3. 其他节点 write 能 invalidate 当前 dirty owner。
4. unmap、fence、shutdown 前 dirty line 必须 writeback。
5. `COH_FENCE_ACK` 返回前，目标 range 已到 persistent point。

## 11. 里程碑和测试支架

这些阶段不是改变最终目标，而是降低实现风险。每个阶段都应保留 CLI 和测试，作为后续 MESI 的回归支架。

### 11.1 Milestone A: write-through coherent profile

目标：先打通 normal cacheable mmap + `obmm.skip_cache_maintain=1` 用户路径。

实现：

1. 新增 `DIRECTORY_MESI` map policy，但内部暂时以 write-through 实现。
2. CPU write 立即写 home backing。
3. CPU read 不使用 stale read cache。
4. DMA read/write 走同一 coherence API。

价值：

1. 验证 import/mmap/cache_policy/QEMU route 正确。
2. 验证 guest 不调用 cache maintenance 时用户可见数据仍正确。
3. 为最终 MESI 保留相同入口和测试。

限制：

1. 没有 `M` dirty owner。
2. 没有 sharer invalidation。
3. 性能不是最终目标。

### 11.2 Milestone B: fence/drain/persistent-point

目标：建立可验证的 delivered-to-target 和 persistent-point 语义。

实现：

1. 新增 `COH_FENCE/COH_FENCE_ACK`。
2. 新增 `COH_WB/COH_WB_ACK`，即使此阶段还没有完整 MESI，也用于测试 writeback ack。
3. `OBMM_SHMDEV_SYNC_REMOTE_RANGE` 在 MESI policy 下转成 coherence fence。

价值：

1. 后续 MESI 需要同样的 ack 和 drain 机制。
2. 可以定位数据错误是 route、writeback、invalidation 还是 ordering 问题。
3. unmap/shutdown 可以依赖 fence 保证 dirty data 落到 persistent point。

### 11.3 Milestone C: shared-read state

目标：实现 `I/S/E` 的 read side。

实现：

1. `GETS` 创建 shared owner set。
2. 多节点读同一 line 后 directory 进入 `S`。
3. 没有 writer 时读不反复 remote read。

价值：

1. 证明 directory 和 sharer bitmap 正确。
2. 先实现只读共享场景，风险低于直接引入 dirty owner。

### 11.4 Milestone D: exclusive write and invalidation

目标：实现 `GETM` 和 sharer invalidation。

实现：

1. writer 获取 exclusive ownership。
2. directory 向所有 sharers 发送 `INV`。
3. 收齐 `INV_ACK` 后授予 `M`。
4. reader 再读时必须重新 `GETS`。

价值：

1. 证明 cross-node invalidation 正确。
2. 覆盖写后读、读后写、多 reader 后 single writer。

### 11.5 Milestone E: full MESI dirty owner

目标：完整支持 `M` dirty owner 和 owner-to-home writeback。

实现：

1. local write 命中 `E` 时升级为 `M`。
2. remote read 命中 dirty owner 时触发 owner writeback 或 data-forward。
3. remote write 命中 dirty owner 时先回收 dirty line，再转移 ownership。
4. unmap/fence/shutdown 必须 flush dirty lines。

价值：

1. 达成最终目标。
2. `obmm.skip_cache_maintain=1` 不再依赖 write-through 保守路径。

## 12. 测试设计

每个功能必须同时有 CLI 和测试用例。

建议新增 guest CLI：

```text
linqu_ub_obmm_coh_test
```

核心参数：

```text
--nodes <N>
--mode <write-through|mesi>
--cache-policy <nc|write-through|directory-mesi>
--pattern <single-writer|multi-reader|read-after-write|write-after-read|pingpong|dma-mixed>
--line-size <64>
--iterations <N>
--verify
--fence <none|every-op|periodic|final>
```

必测场景：

1. `single_writer_remote_reader`：node A 写，node B 读。
2. `multi_reader_shared`：多个节点读同一 line，验证 `S` sharers。
3. `writer_invalidates_readers`：多个 reader 后 writer 写，验证 reader stale cache 被 invalidated。
4. `dirty_owner_remote_read`：owner 处于 `M`，其他节点读，验证 writeback/data-forward。
5. `dirty_owner_remote_write`：owner 处于 `M`，其他节点写，验证 ownership transfer。
6. `dma_mixed_cpu_window`：CPU window 和 strict DMA 混合访问同一 range。
7. `unmap_flushes_dirty`：dirty line 后 unmap，home backing 必须是最新。
8. `skip_cache_maintain_enabled`：明确启动 `obmm.skip_cache_maintain=1`。

QEMU 日志验收：

```text
OBMM_COH_MAP
OBMM_COH_GETS
OBMM_COH_GETM
OBMM_COH_INV
OBMM_COH_INV_ACK
OBMM_COH_WB
OBMM_COH_WB_ACK
OBMM_COH_FENCE
OBMM_COH_FENCE_ACK
OBMM_COH_STATS
```

## 13. 失败处理

1. `GETM` 收不到所有 `INV_ACK`：返回 backend error，directory line 标记为 error/pending，不能静默授予 `M`。
2. dirty owner writeback 失败：fence/unmap 必须失败，不能删除 directory state。
3. node disconnect：home directory 必须 invalidate 该 node 的 sharer/owner 状态；如果该 node 是 dirty owner，则对应 range 进入 data-lost/error 状态。
4. overlapping map：必须拒绝或合并到同一个 coherence map，不能创建两个互相不知道的 directory entry。

## 14. 推荐实现顺序

1. 新增 `OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI` ABI 和 map plumbing。
2. 新增 QEMU `obmm_coherence.{c,h}`，先实现 write-through profile。
3. 把 CPU window 和 strict DMA 命中路径都切到 `obmm_coh_*`。
4. 新增 `COH_FENCE` 和 persistent-point 测试。
5. 实现 `GETS/S`。
6. 实现 `GETM/INV/INV_ACK`。
7. 实现 `M` dirty owner、writeback 和 ownership transfer。
8. 加入 unmap/shutdown dirty flush。

## 15. 设计结论

完整目标就是 directory-based MESI。write-through 和 fence/drain 不应被当成最终产品语义，而应作为开发里程碑和测试支架。

这样分阶段的原因是：先固定用户入口和 data path，再固定 persistent-point 和 ack 语义，最后实现复杂的 ownership 状态机。每个阶段都让系统更接近最终目标，并且留下可回归的 CLI 和测试，避免 MESI 问题和 import/mmap/route 问题混在一起。
