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
4. 不承诺拦截 export node guest 对原始 OBMM backing 的普通 direct mmap 并发写；这类写不经过 imported CPU window 或 UBC DMA path，QEMU directory 无法自动观察。需要 coherent 的 home-side writer 必须通过 directory-aware path，或者在测试中只在 publish 前初始化 backing。

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
#define OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE      2
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK      3
#define OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI  4
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
COH_DOWNGRADE     downgrade clean exclusive owner to shared
COH_DOWNGRADE_ACK downgrade completed
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
OBMM_COH_DOWNGRADE
OBMM_COH_DOWNGRADE_ACK
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

## 15. 当前实现状态

QEMU backend 当前实现已经从 milestone stub 进入 directory-based MESI 路径；最终语义以 guest runtime acceptance 和 QEMU coherence 日志共同验收：

1. `DIRECTORY_MESI` cache policy 已接入 GVA map 校验、CPU window read/write 和 strict DMA read/write。
2. `obmm_coherence.c` 维护 per-node local coherent cache 和 home directory。
3. local cache key 包含 `BusControllerDev *`、`home_cna`、`token_id` 和 line address，避免同一 QEMU 进程内多个 UBC 互相污染。
4. directory key 包含 `home_cna`、`token_id` 和 line address，避免不同 export/home 或 token 的 line 混用。
5. coherence 消息已覆盖 `GETS`、`GETM`、`DATA`、`INV`、`INV_ACK`、`DOWNGRADE`、`DOWNGRADE_ACK`、`WB`、`WB_ACK`、`FENCE`、`FENCE_ACK`。
6. pending response 不再使用单个全局 wait slot，而是按 `(ubc_dev, req_id, peer_cna, msg_type)` 匹配 outstanding 请求。
7. `GETS` 支持 `S/E` grant，`E` 可本地静默升级为 `M`。
8. `GETS` 遇到 clean `E` owner 时会 downgrade 到 `S`，从而建立多 reader shared state；`GETM` 会 invalidate owner 和 sharers，并等待 ACK 后才授予 `M`。
9. dirty owner invalidate 会先撤销本地权限，writeback 成功后保持 `I`；writeback 失败会恢复原 local line，避免丢失唯一 dirty copy。
10. `COH_WB` 只接受当前 directory owner 的 writeback，拒绝 unsolicited、stale 或重复 owner 不匹配的 writeback。
11. `COH_WB` directory 更新采用先确认 sharer 可记录、再清 owner 的提交顺序，避免失败 ACK 同时破坏 home owner 状态。
12. `COH_FENCE` 会 drain home range；home 为本节点时直接本地 drain，不向自己发送 UB Link 消息。
13. drain 只要求 dirty owner 到达 persistent point；clean `E/S` owner 不需要 writeback。requester-side `COH_FENCE` 会先 flush 本地 dirty `M` line，home-side range drain 只处理 directory 标记为 dirty 的 owner line，避免大范围 fence 因 clean owner 无需落盘却被反复 drain 而超时。
14. requester-side `COH_FENCE` flush local dirty `M` line 时会按 `token_id` 过滤，避免同一 home/range 下不同 OBMM token 的 local line 被错误降级或 writeback。
15. home-side range drain 会持续处理 range 内所有 owner line 直到为空，不再用 sharer 数量作为循环上限，避免大 range fence 因 owner line 数超过 `OBMM_COH_MAX_SHARERS * 2` 而失败。
16. `obmm_coh_read/write` 的跨 cache-line 分割使用 `len > line_size - line_off` 形式，避免 `line_off + len` 溢出导致异常大访问被误当作单 line 处理。
17. DMA 命中 `DIRECTORY_MESI` 后进入 coherence API 前会拒绝超过 `uint32_t` 长度的请求，避免 `size_t` 到 `uint32_t` 截断。
18. SIM_DEC/GVA map 会拒绝 overlapping PA range 和 explicit GVA route range；range end 计算使用溢出检查，避免 `base + size` wrap 后绕过 overlap 检查并创建互相不知道的 directory route。
19. GVA ownership registry 注册只会替换同一 `(node_id, map_id, gva_id)` 的记录，不会因为 `map_id` 碰撞误删其他 GVA 记录；registry 中非法或溢出的 range 按冲突 fail-closed 处理。
20. home directory 的 `pending` 字段现在用于串行化同一 line 的 `GETS`、`GETM`、home-side fence drain 和 requester-side home-local dirty flush；line 已 pending 时新请求返回失败而不是并发授予权限，避免同 line 状态机交错提交。
21. home-side fence drain 和 requester-side home-local dirty flush 遇到同 range/line 的 pending owner line 时会失败，而不是覆盖 pending 并继续 writeback/invalidate，避免 fence 和正在进行的 `GETS/GETM` 交错。
22. home node 本地 dirty owner 在 fence/writeback 后会同步 directory owner 状态为 clean sharer；如果 sharer 表无法记录，则保守保持 owner 状态，由后续 drain invalidate 收敛。
23. guest kernel 和 QEMU backend 的 sync/fence range 校验使用 `offset > size || len > size - offset` 形式，避免 `offset + len` wraparound 绕过边界检查；`obmm_coh_send_fence()` 在分配 coherence `req_id` 前先检查 `ubc_dev` 和 overflow range。
24. `UNMAP` 在 `DIRECTORY_MESI` 下必须先 fence，fence 失败则 unmap 失败；fence 成功后会 invalidate 本地 range。
25. SIM_DEC cleanup/shutdown 会在销毁 active `DIRECTORY_MESI` map 前执行 coherence fence，并在成功后 invalidate 本地 range；shutdown 失败路径会记录 QEMU 日志，因为此时已无法向 guest 返回错误。
26. QEMU coherence 日志已提供稳定 token：`OBMM_COH_GETS`、`OBMM_COH_GETM`、`OBMM_COH_DATA`、`OBMM_COH_INV`、`OBMM_COH_INV_ACK`、`OBMM_COH_DOWNGRADE`、`OBMM_COH_DOWNGRADE_ACK`、`OBMM_COH_WB`、`OBMM_COH_WB_ACK`、`OBMM_COH_FENCE`、`OBMM_COH_FENCE_ACK`。
27. `linqu_ub_obmm_coh_test` 已加入 initramfs 构建，用于覆盖当前 dual-node CLI 可真实表达的 `write_read`、`fence` 和 `read_after_wb` 场景；`multi_reader`、`writer_inv`、`mixed_rw`、`dma_write_read` 不再伪装成已覆盖，避免测试误报完整 MESI。
28. `linqu_ub_obmm_coh_test` 要求 `--size` 是非零 64B 对齐值，避免 `uint64_t` pattern verify 忽略非 line-aligned 尾部，造成测试误报。
29. `linqu_ub_obmm_coh_test` 支持 `--generation`，`all` 模式会为每个子测试使用不同 bootstrap generation，避免 stale export record 污染后续子测试。
30. `run_ub_dual_node_obmm_coh_test.sh` 会自动启动双节点 QEMU、传入 `obmm.skip_cache_maintain=1`、执行 `linqu_ub_obmm_coh_test`，并按 mode 检查 `cache_policy=4` 与 `OBMM_COH_GETS`/`OBMM_COH_FENCE`/`OBMM_COH_WB` QEMU 日志证据；默认 mode 为 `write_read` smoke，其他 mode 需显式指定。

已经完成的 runtime acceptance：

1. QEMU 构建已通过。
2. guest kernel artifact 已使用 `ll@192.168.64.3` 重新编译 `Image`，并通过本地 `build_guest_artifacts.sh` 导入到 initramfs。
3. dual-node coherence runner 已在 `obmm.skip_cache_maintain=1` 下通过：

```sh
./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh --mode write_read
./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh --mode fence
./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh --mode read_after_wb
./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh --mode all
```

这些命令分别验证 imported normal-cacheable read/write path、fence persistent-point path、dirty importer writeback 后 exporter backing 可见，以及组合模式。
4. stress acceptance 已通过：

```sh
UB_SYNC_ARTIFACTS=0 STRESS_DIRECTORY_MESI_ACCEPTANCE=1 ./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_import_stress.sh
```

5. 已观察到的关键日志证据包括 `GVA_S3_MAP cache_policy=4`、`OBMM_COH_GETS`、`OBMM_COH_GETM`、`OBMM_COH_WB`、`OBMM_COH_WB_ACK`、`OBMM_COH_FENCE` 和 `OBMM_COH_FENCE_ACK`。

仍需补齐的测试覆盖：

1. 当前 `linqu_ub_obmm_coh_test` 覆盖 dual-node 可真实表达的 `write_read`、`fence`、`read_after_wb` 和 `all`。
2. section 12 中的 `multi_reader_shared`、`writer_invalidates_readers`、`dirty_owner_remote_write` 和 `dma_mixed_cpu_window` 仍需要后续增加 3-node/4-node 或 DMA-aware 测试支架。
3. 这些是测试覆盖缺口，不应在 CLI 中伪装成已验证；实现变更需要等对应 runtime test 和日志断言一起补齐。

## 16. 设计结论

完整目标就是 directory-based MESI。write-through 和 fence/drain 不应被当成最终产品语义，而应作为开发里程碑和测试支架。

这样分阶段的原因是：先固定用户入口和 data path，再固定 persistent-point 和 ack 语义，最后实现复杂的 ownership 状态机。每个阶段都让系统更接近最终目标，并且留下可回归的 CLI 和测试，避免 MESI 问题和 import/mmap/route 问题混在一起。
