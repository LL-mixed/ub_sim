# GVA Simulation Design on Current UB Sim

## 1. 目标与边界

本文目标：在当前 `ub_sim` 已经实现的 guest/QEMU/host UB 仿真基础上，如何加入对灵衢 `GVA` 的模拟实现。

这里的 `GVA` 指全局虚拟地址，数据面访存路径：

```text
PU LD/ST/DMA
  -> *MMU produces UBA from VA + PTE.offset
  -> MMU.S3 looks up {VMID, ASID, UBA}
  -> NoC/UBC routes by {dcna, TID, UPI, p_tag}
  -> UB Link
  -> Target UBC/UMMU validates {EID, TID, UBA}
  -> Target memory
```

当前设计的最终目标不是在 `SIM_DEC` 下游补一个“虚假的 decoder”，而是在 QEMU 中建立可演进到真实 `*MMU.S3/NoC` 位置的 GVA 模型。第一阶段为了复用已经闭合的 UB Link 数据面，会继续使用现有 `SIM_DEC` 代码作为内部 backend；但从架构语义上看，`SIM_DEC` 不是 GVA 组成部分，只是分阶段实现中的 legacy transport/helper。

非目标：

1. 第一阶段不新增完整 CPU MMU 模型。
2. 第一阶段不在 QEMU TCG 中拦截任意 guest VA load/store。
3. 不承诺第一阶段支持真实 cache coherent GVA。
4. 第一阶段不替代现有 `OBMM`、`UB Sim Decoder`、`UB Link`、`UMMU` 仿真路径。

核心原则：

1. 复用当前已闭合路径，不重写一条旁路协议。
2. 保留 `EID/Token/UBA/CNA` 语义链，不能退化为 QEMU 私有 `VA -> GPA` 表。
3. 地址管理和路由模拟分层：`GVA Manager` 负责全局地址分配/保留，`GVA Simulation Layer` 负责 `MMU.S3/NoC` route 和 QEMU backend。
4. 管控面必须可追踪：每条 GVA 映射都能回溯到 `GVA Manager`、guest `OBMM` 或后续 `GVA control` 命令。
5. 数据面必须可验证：读写结果必须由 QEMU/guest 日志、统计计数和端到端测试证明。

## 2. 当前已实现基线

### 2.1 guest 侧

当前 guest Linux 已经具备以下基础：

1. `OBMM_CMD_EXPORT` 可以导出内存并返回：
   - `mem_id`
   - `uba`
   - `tokenid`
2. `OBMM_CMD_IMPORT` 支持导入远端内存，并通过 `priv` 携带仿真专用字段：
   - `remote_uba`
   - `token_value`
3. `obmm_import.c` 已有 sim decoder callback 机制：
   - `obmm_register_import_callback`
   - `obmm_register_unimport_callback`
4. `ub-sim-decoder` 模块已实现：
   - `ub_sim_decoder_service`
   - `ub_sim_decoder_ctrl_adapter`
   - `SIM_DEC_OP_MAP/UNMAP/SYNC/QUERY`
   - OBMM import/unimport callback 注册
5. `obmm_shm_dev.c` 已可通过 `OBMM_SHMDEV_SYNC_IMPORT_RANGE` 触发 sim decoder `SYNC`。

这意味着当前 guest 已有一个可用的管理面入口：

```text
OBMM export/import
  -> obmm sim-dec callback
  -> ub-sim-decoder service
  -> hisi private msg
  -> QEMU SIM_DEC backend
```

### 2.2 QEMU 侧

当前 QEMU UB backend 已经具备：

1. `SIM_DEC` 控制协议：
   - `SIM_DEC_OP_MAP`
   - `SIM_DEC_OP_UNMAP`
   - `SIM_DEC_OP_SYNC`
   - `SIM_DEC_OP_QUERY`
   - `SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH`
   - `SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP`
2. `SimDecMapEntry` 映射表，记录：
   - `local_pa`
   - `size`
   - `remote_uba`
   - `token_id`
   - `token_value`
   - `scna/dcna`
   - `seid/deid`
   - `upi/src_eid`
3. `memory_region_init_io` 建立 imported PA CPU window。
4. CPU window read/write 会转换为远端 `UBA` read/write。
5. strict DMA data path 中 `sim_dec_lookup_by_pa()` 命中后，会直接改发远端 `UBA` 访问。
6. UB Link 已承载跨节点 `SIM_DEC_WRITE/READ_REQ/READ_RESP/BATCH` 数据面消息。
7. 目标侧 QEMU 使用 strict local DMA/UMMU 语义执行读写。

当前 QEMU 已经模拟了 GVA 目标路径中的关键效果：

```text
local imported PA window
  -> decoder map lookup
  -> remote_uba + offset
  -> UB Link message
  -> target strict DMA access
```

### 2.3 原始基线缺口

本文档立项时的实现仍更像“UB Decoder/OBMM 直访仿真”，还不是完整的 “GVA 仿真”。当时的主要缺口是：

1. 没有显式 `GVA map` 对象，`remote_uba` 仍由 OBMM 私有 `priv` 隐式携带。
2. 没有独立 `GVA Manager` 负责全局地址空间、GSVA reserved range 和地址生命周期。
3. 没有模拟 `VA + PTE.offset -> UBA` 的可观测计算过程。
4. 没有模拟 `{VMID, ASID, UBA} -> {dcna, TID, UPI, p_tag}` 的 `MMU.S3 ma_table` 语义。
5. 没有模拟 `p_tag -> UBC port/lane` 的 NoC `mp_table` 语义。
6. `cacheable`/`non-cacheable` 策略没有作为 GVA 映射属性统一表达。
7. 现有日志和统计以 `SIM_DEC` 为中心，不足以回答“这次访问是否走了 GVA 模拟路径”。

### 2.4 代码基线校验

本设计基于当前代码做过以下核对：

1. OBMM import callback 真实存在。
   - 文件：`guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c`
   - 回调签名：`int (*import_fn)(void *)`、`int (*unimport_fn)(void *)`
   - 注册函数：`obmm_register_import_callback()`、`obmm_register_unimport_callback()`
   - `obmm_import()` 的顺序是 `prepare_import_memory()` 成功后调用 sim decoder map；后续注册 OBMM region 失败时会 rollback 调用 unmap。
2. 当前 `OBMM_CMD_IMPORT` 通过 `priv` 携带仿真字段。
   - UAPI：`guest-linux/kernel_ub/include/uapi/ub/obmm.h`
   - `struct obmm_cmd_import` 有 `priv_len` 和 `const void *priv`。
   - `OBMM_MAX_PRIV_LEN` 当前为 `512`。
   - 当前私有结构：`struct obmm_sim_dec_import_priv_v1`
   - 字段：`magic/version/len/remote_uba/token_value/flags`
3. guest sim decoder 控制协议是真实 ABI。
   - 文件：`guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder.h`
   - `SIM_DEC_PROTO_VERSION` 当前为 `1`。
   - `struct sim_dec_map_req` 是 packed wire-equivalent payload 的 guest 侧定义。
   - 字段只有 `local_pa/size/remote_uba/token_id/token_value/scna/dcna/seid/deid/upi/src_eid`。
4. QEMU 侧 `SimDecMapEntry` 当前只在 `ub_ubc.c` 内定义。
   - 文件：`vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
   - 没有独立 public header 承载该结构。
   - `memory_region_init_io()` 建立 CPU window 的代码也在同一文件。
   - `sim_dec_lookup_by_pa()` 签名在 QEMU header 中导出，当前返回 `remote_uba/token_id/src_eid/dcna`。
5. UB Link 数据面消息格式已经独立于控制面。
   - 文件：`vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h`
   - write payload：`UBCSimDecWritePldHdr { remote_uba, token_id, data_len }`
   - read request：`UBCSimDecReadReqPld { req_id, token_id, remote_uba, read_len }`
   - 当前数据面没有 `vmid/asid/p_tag/cache_policy` 字段。
6. 构建系统当前将 sim decoder 作为 `ubus/sim` 子模块。
   - `drivers/ub/ubus/Makefile` 通过 `CONFIG_UB_UBUS_SIM_DECODER` 引入 `sim/`。
   - `drivers/ub/ubus/sim/Makefile` 生成 `ub-sim-decoder.ko`。
   - initramfs 默认装载顺序中 `obmm.ko` 在 `ub-sim-decoder.ko` 前，当前依赖关系是 sim decoder 后注册 OBMM callback。

上述核对带来一个直接约束：不能把 `vmid/asid/p_tag/cache_policy` 直接塞进现有 `SIM_DEC_OP_MAP` payload 并假设兼容。`SIM_DEC_OP_MAP` 是 guest/QEMU 双端 v1 ABI；Phase A 必须保持 legacy MAP 不变，通过新增 GVA metadata payload 或新 opcode 承载 GVA 语义。

## 3. 目标架构

目标架构必须反映 PPT 中的真实 GVA 位置：`MMU.S3` 在 `*MMU` 内部，NoC `mp_table` 在 UBC/NoC 路由域内；真实架构里不存在一个独立的 decoder 组件。

### 3.1 最终目标架构

最终目标应朝这个方向演进：

```text
Application / workload
  -> normal VA load/store or DMA VA/IOVA
      -> QEMU ARM MMU / TCG translation hook
          -> stage-1 translation obtains VA context and PTE-side GVA metadata
              -> UBA = VA + PTE.offset
                  -> QEMU MMU.S3 ma_table lookup
                      {VMID, ASID, UBA range}
                        -> {dcna, TID, UPI, p_tag}
                      -> QEMU NoC mp_table lookup
                          {p_tag} -> {ubc_port, link, lane}
                          -> GVA MemoryRegion / UB Link backend
                              -> target QEMU UBC/UMMU validation
                                  -> target memory
```

在这个目标架构中：

1. `MMU.S3` 属于地址翻译路径，而不是 imported PA window 的后处理。
2. `ma_table` 的主键是 `{VMID, ASID, UBA}`，结果是 `{dcna, TID, UPI, p_tag}`。
3. `mp_table` 的主键是 `p_tag`，结果是可观测的 UBC port/link/lane。
4. 数据面最终不应该暴露 `SIM_DEC` 这个概念；它只应该看到 `gva_read/gva_write/gva_dma`。
5. 现有 UB Link remote read/write 可以继续作为物理传输 backend。

### 3.2 为什么第一阶段不直接改 QEMU TCG/ARM MMU

通过修改 QEMU TCG/ARM MMU 来实现 S3 在技术上可行，但不适合作为第一阶段主路径。原因是这会把问题从“GVA 语义模拟”升级为“改 QEMU ARM 地址翻译体系”：

1. 当前 guest Linux 普通 PTE 没有 `PTE.offset` 语义。
   - 要么扩展 guest page table/PTE encoding。
   - 要么额外维护 side table。
   - 否则 ARM MMU 翻译时拿不到 `VA + PTE.offset` 所需的 offset。
2. QEMU TCG TLB fast path 会缓存翻译结果。
   - S3 lookup 不能只在慢路径查一次。
   - `ma_table` 更新、unmap、ASID/VMID 切换、权限变化都必须触发正确的 TLB invalidation。
   - 否则 guest load/store 可能继续命中旧 route。
3. QEMU MMU 翻译结果天然是 host 可调度的 `MemoryRegionSection` 或物理地址语义。
   - GVA S3 返回的是 `{dcna, TID, UPI, p_tag}`，不是普通 PA。
   - 需要把 S3 结果编译为特殊 aperture 或 GVA `MemoryRegion`，再让 read/write callback 带 route metadata 走 UB Link。
4. 改动会侵入 `target/arm` 翻译逻辑。
   - 任意 bug 都可能表现为 Linux 随机 page fault、TLB stale、DMA 地址错误或内存破坏。
   - 调试成本高于在 UB backend 边界做功能级建模。

因此第一阶段应先做“功能级 MMU.S3/NoC 模型”，把 GVA 的控制面、路由表、权限、统计和故障注入做实。等语义稳定后，再把入口从 imported PA window/DMA hit 前移到 QEMU ARM MMU/TCG translation hook。

### 3.3 分阶段实现架构

在开始实现前先锁定一个关键顺序假设：
`GVA Manager` 的 bootstrap 必须建立在已可用的 OBMM bootstrap 之上（仅用于 manager control plane 初始化与范围协商），该依赖不在本阶段的 QEMU backend 内实现。
`GVA Simulation Layer` 本身只消费 GVA 管理面产生的 map 请求和元数据，当前第一阶段不重新实现 OBMM bootstrap 或 MPMC 队列。

第一阶段新增 `GVA Simulation Layer`，位于 guest `OBMM/UBMM` 与现有 UB Link backend 之间。这里仍会复用 `SIM_DEC` 代码路径，但只能把它视为 legacy backend，而不是 GVA 架构组件。

```text
Application / workload
  -> libgva / mmap / load-store / DMA
      -> GVA Manager or legacy OBMM / UBMM control plane
          -> GVA Simulation Layer
              -> QEMU MMU.S3 ma_table model
              -> QEMU NoC mp_table model
              -> GVA permission/cache policy model
              -> internal backend command
                  -> legacy SIM_DEC map/read/write implementation
                      -> CPU window / strict DMA hit
                          -> UB Link
                              -> target QEMU UMMU/memory
```

`GVA Simulation Layer` 的职责是把 PPT 中的 GVA 概念显式化，并把它们编译到当前 QEMU 已有的 UB Link 数据面：

1. 消费上游 `GVA Manager` 或 legacy OBMM import 产生的 `GVA map request`。
2. 建立 `GVA map`，保存 `VA/PTE.offset/UBA` 关系。
3. 建立 `ma_table` 模型，保存 `{VMID, ASID, UBA range} -> {dcna, TID, UPI, p_tag}`。
4. 建立 `mp_table` 模型，保存 `{p_tag} -> {ubc_port, link, lane}`。
5. 把上述模型编译成当前 QEMU 可执行的 internal backend map。
6. 在数据面命中时输出 `gva_path` 级别的观测记录。

`GVA Simulation Layer` 不负责选择全局地址。地址来源必须来自：

1. `GVA Manager`：管理 GSVA 或后续普通 GVA 的全局地址空间。
2. legacy OBMM import：仅作为 Phase A 兼容入口，使用 OBMM export 返回的 `remote_uba` 生成默认 GVA metadata。

### 3.4 Global Address Management

完整 GVA 需要一个独立的 `GVA Manager`，运行在 UB-connected supernode 上的每一个 OS 中。它是 GVA 控制面的上游地址管理组件，不是 QEMU backend。

`GVA Manager` 的职责：

1. 在 bootstrap 阶段通过基于 OBMM shmem 的 MPMC 队列与 peer manager 协商全局地址策略。
2. 为 GSVA 协商并 reserve 一段所有参与 OS 都可用的 global VA range。
3. 将 reserved range 注册到 guest kernel 和 OBMM 地址管理机制，避免普通 VA/mmap 和 OBMM shmdev mapping 误占。
4. 从 reserved range 或普通 GVA address pool 中分配 GVA segment。
5. 调用 OBMM export/import 或 GVA control API，把 `{local_va, pte_offset, uba_base, home, token}` 编译为 `ub_gva_map_req`。
6. 维护 segment lifetime、generation、retire/reuse fence。

GVA 与 GSVA 的关系：

```text
GVA Manager
  -> produces ub_gva_map_req
      -> GVA Simulation Layer
          -> MMU.S3 ma_table / NoC mp_table
              -> QEMU backend / UB Link
```

普通 GVA profile 允许：

```text
UBA = User VA + PTE.offset
pte_offset may be nonzero
```

GSVA 是 GVA Manager 的 strict identity profile，要求：

```text
user_va == uba == home_va
pte_offset == 0
```

如果 GSVA reserved range 不能被 guest kernel/OBMM 同时保留，GSVA session 必须失败。不能通过 `pte_offset != 0` relocation 或 QEMU private alias 继续伪装为 GSVA；那只能退化为普通 GVA。

术语约束：

1. `GSVA reserved VA aperture` 指 guest OS/进程 VA 层面的全局保留区间。
2. `QEMU GVA MemoryRegion aperture` 指 QEMU 数据面可命中的 dispatch 入口。
3. 两者可以由同一个 `ub_gva_map_req` 关联，但不能混为同一层地址管理对象。

这个分阶段设计的关键约束：

1. 用户、guest API、日志和测试应逐步使用 `GVA/MMU.S3/NoC` 术语，不再把 `SIM_DEC` 当成目标架构。
2. `SIM_DEC` 可以作为 C 代码、map storage、CPU window callback、DMA fast path 的复用实现存在。
3. 新增接口不得把 `SIM_DEC` 固化成 GVA 的长期 ABI。
4. 后续切到 QEMU ARM MMU/TCG hook 时，`GVA Manager`、`ma_table/mp_table` 与统计/测试应可复用，只替换入口路径。

## 4. 关键抽象

### 4.1 GVA Map

`GVA Map` 是 guest 可见远端内存映射的主对象。

建议结构：

```c
struct ub_gva_map_req {
    u64 local_va;
    u64 home_va;
    u64 local_pa;
    u64 size;
    u64 pte_offset;
    u64 uba_base;
    u32 vmid;
    u32 asid;
    u32 token_id;
    u32 token_value;
    u32 scna;
    u32 dcna;
    u32 upi;
    u32 src_eid;
    u8  seid[16];
    u8  deid[16];
    u32 cache_policy;
    u32 access_flags;
    u32 map_source;
    u32 address_profile;
};
```

字段含义：

1. `local_va`：用户侧 VA。legacy OBMM Phase A 兼容入口可为 `0`，因为当前路径从 mmap/imported PA 窗口触发；`GVA Manager` 入口必须填写真实 VA，GSVA 中必须等于 `gsva_base`。
2. `home_va`：Home 侧 VA。普通 GVA 可为 `0` 或仅用于调试；GSVA 中必须等于 `local_va` 和 `uba_base`。
3. `local_pa`：当前 `SIM_DEC` 的命中地址，必须保留。
4. `pte_offset`：模拟 PPT 中 `PTE.offset`。
5. `uba_base`：模拟 `VA + PTE.offset` 生成的 `UBA`；legacy Phase A 可等于 OBMM export 返回的 `remote_uba`，GSVA 中必须等于 `local_va` 和 `home_va`。
6. `vmid/asid`：模拟 `MMU.S3` 查找上下文；第一阶段允许默认值，但必须进入表项和日志。
7. `cache_policy`：显式表达 `Normal NC`、`cacheable read-only`、`write-through`、`write-back` 等策略。
8. `map_source`：区分 `legacy_obmm` 和 `gva_manager`。
9. `address_profile`：区分 `generic_gva` 和 `gsva_identity`。`gsva_identity` 必须强校验地址三等值。

### 4.2 GVA Route Entry

`GVA Route Entry` 对应 PPT 中 `ma_table` 的仿真形态。

建议结构：

```c
struct ub_gva_route_entry {
    u32 vmid;
    u32 asid;
    u64 local_va;
    u64 home_va;
    u64 uba_base;
    u64 size;
    u32 dcna;
    u32 tid;
    u32 upi;
    u32 p_tag;
    u32 address_profile;
    u64 sim_dec_map_id;
};
```

查找键：

```text
{VMID, ASID, UBA}
```

输出：

```text
{dcna, TID, UPI, p_tag, sim_dec_map_id}
```

第一阶段中，真实数据面仍由 `local_pa -> SimDecMapEntry` 命中；但控制面必须维护这张 `GVA route table`，并在日志中证明该 backend map 是从哪条 `GVA route entry` 编译而来。

### 4.3 GVA NoC Port Entry

`GVA NoC Port Entry` 对应 PPT 中 `mp_table` 的仿真形态。

建议结构：

```c
struct ub_gva_mp_entry {
    u32 p_tag;
    u32 local_cna;
    u32 peer_cna;
    u32 ubc_port;
    u32 lane;
    u32 link_id;
};
```

第一阶段不要求改变 QEMU 真实路由算法，也不要求 guest 侧读取完整 FM link state。`p_tag` 支持两种来源：guest/GVA layer 可显式给出非零 tag；若请求中 `p_tag=0`，QEMU backend 在解析 FM neighbor 后派生 effective `p_tag=mp_table.link_id`。Phase B 再校验 effective `p_tag -> {port,lane,link}`，显式非零 `p_tag` 与 resolved link 不一致时必须触发 route miss。

### 4.4 Phase A 接口决策

为了让 Phase A 可以编码，以下问题先按固定策略处理：

1. `VMID/ASID` 来源：
   - Phase A 默认 `vmid=0`、`asid=0`。
   - 可允许测试 CLI/UAPI 显式传入非零值，用于 route key 覆盖测试。
   - 不从 `task_struct` 或 `mm_struct` 推导，避免把第一阶段绑定到 Linux 进程地址空间实现。
2. `PTE.offset` 来源：
   - Phase A 不改 guest page table。
   - `pte_offset` 作为 GVA control metadata 字段。
   - 普通 GVA profile 是 identity-first：优先让 `User VA == UBA`，此时 `pte_offset=0`。
   - 若普通 GVA 中 User 侧与 `UBA` 等值的 VA 不可用，例如已被占用、不满足 mmap 约束、ASLR/布局冲突，或测试显式要求 relocation，则选择其他 `User VA`，并记录 `pte_offset=UBA-User VA`。
   - GSVA profile 不允许 relocation；`GVA Manager` 必须先 reserve 可用的 `gsva_base`，并保证 `local_va=uba_base=home_va=gsva_base`、`pte_offset=0`。
   - Phase A 默认 `local_va=0`、`pte_offset=0`、`uba_base=remote_uba`，因为当前尚未接入 QEMU ARM MMU/TCG VA translation。
   - 这里的 `pte_offset` 只描述 `User VA -> UBA` 的重定位关系；只有 GSVA profile 才额外要求 `Home VA == UBA`。
3. `p_tag` 来源：
   - Phase A 允许 GVA layer 显式静态分配非零 `p_tag`。
   - 若请求 `p_tag=0`，表示由 QEMU backend 自动派生；backend 根据 FM neighbor 解析出的 `mp_table.link_id` 写入 route entry 的 effective `p_tag`。
   - Phase B 校验 effective `p_tag -> {port,lane,link}`；显式非零 `p_tag` 与 resolved link 不一致时触发 `p_tag_mismatch`。
4. `cache_policy` 范围：
   - Phase A 只支持 `NC/write-through`。
   - QEMU 当前 `SIM_DEC_WRITE_MODE=write-back` 仍属于 legacy backend 调试开关，不能等价为 GVA cacheable。
   - `read-cache`、`write-back`、`MRSW` 延后到 Phase D。
5. `OBMM_CMD_IMPORT` 是否改 UAPI：
   - Phase A 不改 `struct obmm_cmd_import`。
   - 继续使用 `priv`，新增 `OBMM_SIM_DEC_PRIV_VER_2` 承载 GVA metadata。
   - legacy `OBMM_SIM_DEC_PRIV_VER_1` 继续兼容，只生成默认 GVA metadata。
   - 等 Phase A 验证通过后，再评估是否把 `uba/token_value/gva_flags` 升级为显式 UAPI 字段。
6. `SIM_DEC_OP_MAP` 是否扩展：
   - Phase A 不修改现有 `SIM_DEC_OP_MAP` payload。
   - 新增 `SIM_DEC_OP_GVA_MAP = 0x07` 承载 GVA metadata sideband。
   - 新 payload 应包含原 `sim_dec_map_req` 加 `SimGvaRouteMeta`。
   - QEMU 收到后创建同一个 legacy backend map，并在 `SimDecMapEntry` 上挂 `gva_meta`。

## 5. 控制面流程

### 5.1 Export

普通 GVA 的 legacy Phase A export 保持不变：

```text
Home guest
  -> OBMM_CMD_EXPORT
      -> returns {mem_id, uba, tokenid}
```

普通 GVA 新增要求：

1. export 结果必须可转换为 `GVA export descriptor`。
2. descriptor 至少包含 `{deid, export_cna, uba_base, size, token_id}`。
3. 通过现有 `OBMM_BOOTSTRAP_PUBLISH/LOOKUP` 分发时，记录应升级为 `GVA-capable` 语义。

GSVA export 不以 OBMM 自行返回的任意 `uba` 为起点。它必须由 `GVA Manager` 先分配 `gsva_base`，并要求 guest kernel/OBMM 以该地址作为 architectural UBA：

```text
GVA Manager
  -> allocate gsva_base from reserved range
  -> OBMM_CMD_EXPORT with GSVA metadata
      -> returns {mem_id, uba=gsva_base, tokenid}
```

如果 OBMM/UMMU 当前实现无法让 `cmd_export.uba == gsva_base`，GSVA export 必须失败；不能使用 QEMU private alias 掩盖地址不一致。

### 5.2 Import

当前路径：

```text
User guest
  -> OBMM_CMD_IMPORT(priv.remote_uba)
      -> obmm_sim_dec_map_import()
          -> ub_sim_decoder_map()
              -> SIM_DEC_OP_MAP
```

目标路径：

```text
User guest or GVA Manager
  -> OBMM_CMD_IMPORT / UB_GVA_CMD_MAP / GSVA map API
      -> build ub_gva_map_req
          -> allocate GVA route entry
              -> allocate/derive p_tag entry
                  -> compile to internal backend map
                      -> Phase A uses SIM_DEC_OP_GVA_MAP or GVA sideband over SIM_DEC
                      -> QEMU map ack
                          -> import success
```

失败语义：

1. 任一阶段失败必须回滚已创建的 `GVA route entry` 和 backend map。
2. backend map 失败不能注册 OBMM region。
3. route overlap、token mismatch、cache policy 不支持必须返回明确 errno。
4. GSVA map 中 `local_va/uba_base/home_va` 不一致或 `pte_offset != 0` 必须失败，不能降级为普通 GVA。

映射基数：

1. Phase A 强制 `GVA route entry : backend map = 1:1`。
2. 不做按页拆分，不做一个 GVA map 编译为多个 backend map。
3. `gva_map_id` 和 QEMU 返回的 backend `map_id` 都必须记录。
4. 如果未来支持拆分，必须新增 child map list，不能复用 Phase A 的单一字段。

原子性顺序：

1. guest 侧先构造 transient `GVA route entry`，状态为 `creating`。
2. 发送 QEMU backend map。
3. QEMU map 成功后再将 route 状态置为 `active`。
4. route 注册失败时必须立即发送 backend unmap。
5. OBMM region 注册失败时复用现有 rollback 路径 unmap backend，并删除 GVA route。

### 5.3 Sync / Unmap

`SYNC` 继续复用现有 `OBMM_SHMDEV_SYNC_IMPORT_RANGE`，但需要加入 GVA 语义：

```text
OBMM shmdev sync
  -> GVA map lookup
      -> cache policy validation
          -> SIM_DEC_OP_SYNC
```

`UNMAP` 顺序：

```text
OBMM unimport
  -> SIM_DEC_OP_UNMAP
      -> remove GVA route entry
          -> remove GVA map
```

如果 `SIM_DEC_OP_UNMAP` 失败，不能直接删除 GVA 控制面状态；必须保留 `stale/error` 状态，便于后续 cleanup 和诊断。

## 6. 数据面流程

### 6.1 CPU Load/Store

第一阶段可执行路径：

```text
guest load/store imported mmap VA
  -> guest page table maps to imported local PA
      -> QEMU MemoryRegion cpu_window hit
          -> SimDecMapEntry remote_uba + offset
              -> UB Link READ/WRITE
                  -> target QEMU strict DMA
```

新增 GVA 语义：

1. `remote_uba + offset` 必须能反查到 `GVA route entry`。
2. QEMU 日志中需要输出：
   - `gva_map_id`
   - `vmid/asid`
   - `uba`
   - `dcna`
   - `tid`
   - `p_tag`
   - `cache_policy`
3. `SIM_DEC_STATS` 需要增加或派生 GVA 级别计数。

### 6.2 DMA Path

当前 strict DMA data path 已支持：

```text
ubc_dma_read/write_ex(path=DATA, iova)
  -> sim_dec_lookup_by_pa(iova)
      -> ubc_sim_dec_remote_read/write(remote_uba)
```

新增要求：

1. `sim_dec_lookup_by_pa()` 命中后也要关联 `GVA route entry`。
2. 统计上区分：
   - `gva_cpu_reads/writes`
   - `gva_dma_reads/writes`
3. DMA 路径的 `TID` 必须优先来自 GVA route 或 token map，不能隐式丢失。

## 7. Cache Policy

PPT 中对 cacheability 有两个方向：

1. 低时延设计版本强调 User 侧远端访问退化为 `Normal Non-Cacheable`。
2. 高阶服务版本提到 `Multiple Readers, Single Writer` 和 cacheable 读共享。

当前 `ub_sim` 第一阶段采用保守策略：

```text
default: Normal Non-Cacheable / write-through
```

原因：

1. 当前 QEMU/guest 已验证的路径更接近 imported PA window + explicit sync。
2. 尚未实现跨节点 cache coherence 或 ownership invalidation 的硬件级模拟。
3. NC/write-through 最容易证明数据正确性。

后续可分阶段加入：

1. `read-cache shadow`：复用 QEMU 现有 `sync_shadow/page_cache`，只作为显式 cache policy。
2. `write-back + sync`：复用现有 page cache dirty flush，必须强制 `SYNC` 验收。
3. `MRSW ownership`：与 OBMM ownership 状态机绑定，读共享、写独占、写前 invalidation。

## 8. 模块与文件建议

### 8.1 guest kernel

建议新增：

```text
guest-linux/kernel_ub/drivers/ub/ubus/gva/
  ub_gva.h
  ub_gva_service.c
  ub_gva_route.c
  ub_gva_obmm.c
  ub_gva_debugfs.c
```

职责：

1. `ub_gva_service.c`：生命周期、map/unmap/sync API。
2. `ub_gva_route.c`：`ma_table/mp_table` 仿真表。
3. `ub_gva_obmm.c`：OBMM import/export 适配。
4. `ub_gva_debugfs.c`：导出 maps/routes/stats。

Phase A 的放置决策：

1. 先放在现有 `guest-linux/kernel_ub/drivers/ub/ubus/sim/` 下。
2. 先不新增 `drivers/ub/ubus/gva/` 目录。
3. 先不新增独立 `ub-gva.ko`。
4. 在 `ub-sim-decoder.ko` 内新增 `ub_gva_*` 文件，与现有 OBMM callback 共享模块生命周期。

原因：

1. 当前 `drivers/ub/ubus/Makefile` 已通过 `CONFIG_UB_UBUS_SIM_DECODER` 引入 `sim/`。
2. 当前 initramfs 默认装载顺序是 `obmm.ko` 后装 `ub-sim-decoder.ko`，正好满足 sim/GVA 模块注册 OBMM callback。
3. 若新增独立模块，需要同步 `Kconfig/Makefile`、artifact sync、`build_initramfs.sh` module copy、`init.c` 装载顺序，Phase A 没必要扩大这个面。
4. 等 GVA UAPI 和 debugfs 稳定后，再拆为独立 `ubus/gva/` 和 `ub-gva.ko`。

### 8.2 QEMU

建议在 `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` 中先复用现有 `SIM_DEC` storage 和 UB Link read/write helper，但新增代码的抽象名应是 `GVA/MMU.S3/NoC`，而不是继续扩展“decoder”作为架构概念：

```c
typedef struct SimGvaRouteMeta {
    uint64_t gva_map_id;
    uint32_t vmid;
    uint32_t asid;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint64_t uba_base;
    uint64_t size;
    uint32_t dcna;
    uint32_t tid;
    uint32_t upi;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint32_t map_source;
    uint32_t address_profile;
} SimGvaRouteMeta;
```

第一阶段可让 `SimDecMapEntry` 增加可选 `gva_meta`，让现有 CPU window 和 DMA path 继续复用。这里的含义是：

1. `SimDecMapEntry` 是 legacy backend entry。
2. `SimGvaRouteMeta` 才是 GVA 架构语义。
3. QEMU MAP 日志应输出 `GVA_S3_MAP` 或等价前缀，包含 `vmid/asid/local_va/home_va/pte_offset/uba/p_tag/dcna/tid/upi/address_profile`。
4. CPU window 与 DMA path 命中时应先关联 `SimGvaRouteMeta`，再调用已有 remote read/write helper。
5. 后续切换到 QEMU ARM MMU/TCG hook 时，保留 `SimGvaRouteMeta/ma_table/mp_table`，替换掉 imported PA window 入口。

不建议新增一套完全平行的数据面，因为当前 UB Link 消息、remote read/write、strict target UMMU validation 已经是可工作的物理传输层。真正需要替换的是入口和架构命名，而不是底层传输。

### 8.3 User space demos

建议新增或扩展：

```text
guest-linux/aarch64/apps/gva_direct_demo/
guest-linux/aarch64/scripts/run_ub_dual_node_gva_direct_test.sh
guest-linux/aarch64/scripts/run_ub_four_node_gva_matrix.sh
```

所有新增功能必须有 CLI：

1. `gva_direct_demo export`
2. `gva_direct_demo import`
3. `gva_direct_demo read`
4. `gva_direct_demo write`
5. `gva_direct_demo sync`
6. `gva_direct_demo dump`

## 9. 分期计划

### Phase A：GVA 控制面模型与最小数据面观测

目标：把 GVA 概念显式建模，并在不改变底层 UB Link 数据面协议的前提下，证明 CPU window / DMA path 的读写确实命中 GVA route。`SIM_DEC` 只作为内部 backend 复用。

修改：

1. 新增 `GVA map/route/mp` 数据结构。
2. 定义 `map_source=legacy_obmm|gva_manager` 和 `address_profile=generic_gva|gsva_identity`。
3. 在 OBMM import callback 中生成 legacy `ub_gva_map_req`。
4. 为 `GVA Manager` 预留 manager-produced `ub_gva_map_req` 合同，要求填写真实 `local_va/uba_base/pte_offset`。
5. 新增 `SIM_DEC_OP_GVA_MAP` 或 GVA sideband payload；不改变现有 `SIM_DEC_OP_MAP` ABI。
6. 在 guest debugfs 或 sysfs 导出 GVA maps/routes。
7. QEMU `SimDecMapEntry` 增加 GVA metadata。
8. 强制 `GVA route entry : backend map = 1:1`，暂不做 range split。
9. CPU window read/write 输出 `gva_cpu_read/write` 计数。
10. strict DMA path 输出 `gva_dma_read/write` 计数。
11. 增加 `gva_path=cpu_window|dma` 日志。
12. 输出基础 `GVA_STATS`。
13. 新增 `gva_direct_demo`，覆盖 export/import/load/store/sync/unimport/dump。
14. 新增 `run_ub_dual_node_gva_direct_test.sh`。

验收：

1. OBMM import 成功后能看到一条 GVA map。
2. QEMU MAP 日志包含 `gva_map_id/vmid/asid/local_va/home_va/pte_offset/uba/p_tag/address_profile`，且日志前缀体现 `GVA_S3` 而非只有 `SIM_DEC`。
3. `generic_gva` map 允许 `pte_offset != 0` 并在 route dump 中可见。
4. `gsva_identity` map 必须满足 `local_va=uba_base=home_va` 且 `pte_offset=0`，否则 map 失败。
5. 双节点 WRITE 后 Home 侧内存可见。
6. 双节点 READ 返回 Home 侧内容。
7. unmap 后访问失败或返回预期错误。
8. `SIM_DEC_STATS` 与 `GVA_STATS` 均有非零读写计数。
9. `gva_direct_demo --mode=write-read` 通过。
10. `gva_direct_demo --mode=unmap-fault` 通过。
11. `gva_direct_demo --mode=sync` 通过。
12. 现有 dual-node `obmm_demo` 不回退。

### Phase B：MMU.S3 / NoC 表语义增强

目标：从“metadata 附加”升级为“控制面路由模型可校验”，为后续接入 QEMU ARM MMU/TCG hook 做准备。

修改：

1. `ma_table` 模型支持 overlap 检查。
2. `mp_table` 模型从 FM link topology 派生端口。
3. route miss/token mismatch/UPI mismatch 可注入错误。
4. GVA debug CLI 支持 dump route，并展示 request `p_tag`、backend effective `p_tag`、`mp_ubc_port/mp_lane/mp_link_id`。

验收：

1. 错误 `dcna` 或 `p_tag` 触发 route miss。
2. token mismatch 触发读写失败。
3. 八节点场景中每个 peer route 可唯一映射到 active link。
4. 普通 GVA route dump 必须能在 QEMU log 和 guest `/proc/ub_sim_decoder/gva_routes` 中证明 effective `p_tag == link_id`，或者显式错误 `p_tag` 触发 `p_tag_mismatch`。
5. `GSVA_MATRIX_NODE_COUNT=4|8 run_ub_four_node_gsva_matrix_demo.sh` 必须检查每个节点至少有 `node_count - 1` 条 `GVA_S3_MAP address_profile=2`，且每条 route 的 `link_id` 已解析、`p_tag == link_id`、peer `dcna` 唯一、`link_id` 唯一。

### Phase C：QEMU ARM MMU/TCG 入口验证

目标：在不影响默认回归的前提下，验证把 GVA 入口前移到 QEMU ARM MMU/TCG translation hook 的可行性。

修改：

1. `PTE.offset` 的来源采用 GVA route metadata side table：
   - guest 通过 `OBMM_SIM_DEC_PRIV_VER_2` / GVA manager map request 显式下发 `local_va`、`uba_base`、`pte_offset`。
   - QEMU backend 将这些字段保存在 `SimDecMapEntry`，TCG hook 按 `VA + pte_offset` 回查同一条 `ma_table` entry。
   - guest PTE encoding 扩展暂不进入当前 simulator 实现；后续若要模拟真实硬件 PTE bit，再把 side table 替换为 PTE decoder。
2. 在 QEMU ARM translation slow path 中查 `ma_table`。
3. 让 `ma_table` 结果生成可被 memory dispatch 命中的 GVA aperture 或 `MemoryRegionSection`。
4. 对 `ma_table` 更新、unmap、ASID/VMID 切换做 TLB invalidation。
5. 默认关闭该路径，只通过显式环境变量或 machine property 启用。

验收：

1. 单页 GVA load/store 能从 VA 入口命中 S3 route。
2. route 更新后旧 TLB 不再可用。
3. 关闭该路径时现有 OBMM/SIM_DEC/GVA backend 回归不变。
4. 与 Phase A-B 的 `ma_table/mp_table/stats/fault injection` 共用同一套模型。
5. `SIM_GVA_TCG=1 GVA_DIRECT_MODE=write-read run_ub_dual_node_gva_direct_test.sh` 必须看到 `GVA_TCG_TLB_FLUSH reason=gva_map` 和 `GVA_TCG_TRANSLATE`。
6. `SIM_GVA_TCG=1 GVA_DIRECT_MODE=unmap-fault run_ub_dual_node_gva_direct_test.sh` 必须看到 `GVA_TCG_TLB_FLUSH reason=gva_unmap`。
7. `SIM_GVA_TCG=0` 或默认值下不得出现 `GVA_TCG_TRANSLATE`，证明默认路径不侵入 legacy backend 回归。

### Phase D：Cache Policy 与 Ownership

目标：支持 PPT 中的 `MRSW` 方向，但必须以正确性为先。

当前模拟实现采用“拒绝冲突 writer”作为第一步 ownership 语义，而不是伪造远端 invalidation。原因是现有 UB Link 数据面还没有跨 QEMU 进程的 remote reader invalidation 回调，也没有把 OBMM ownership 的页级 reader/writer 状态同步为全局分布式状态。为了让 generic GVA 的 MRSW 在当前仿真里可证明，QEMU GVA backend 对 `address_profile=generic_gva` 的显式 GVA route 在 `UB_FM_SHARED_DIR/gva_ownership/registry.tsv` 维护 host-shared ownership registry：

1. `cache_policy=read_cache` 且 `READ_ONLY` 的 route 注册为 `reader`，允许多个 reader 重叠。
2. 非 `READ_ONLY` 的 generic GVA route 注册为 `writer`。
3. writer 与任意重叠 reader/writer 冲突时，`SIM_DEC_OP_GVA_MAP` 返回 `RESOURCE_BUSY`，日志输出 `GVA_OWNERSHIP_CONFLICT`。
4. route unmap 或 QEMU cleanup 时注销 ownership entry，日志输出 `GVA_OWNERSHIP_UNREGISTER`。

`address_profile=gsva_identity` 不进入这个临时 generic GVA ownership registry。GSVA 的地址生命周期与共享语义由 GSVA Manager、reserved aperture 和 segment owner 管理；否则四节点 GSVA matrix 中每个节点写入其他节点 GSVA slot 时会被 generic writer 独占规则错误拒绝。

这不是最终硬件级 cache coherence；它是当前阶段对“单写者写入前能拒绝或 invalid 其他 writer/reader”的可执行选择：先拒绝，后续若 UB Link/OBMM 增加远端 invalidation 控制消息，再把拒绝升级为 invalidate-and-grant。

修改：

1. `cache_policy=NC` 为默认强制模式。
2. `cache_policy=read_cache` 只允许 read-only map。
3. `cache_policy=write_back` 必须绑定 explicit sync。
4. 与 OBMM ownership 状态连接，写权限获取前 invalid remote readers。

验收：

1. read-only 多节点读共享通过。
2. 单写者写入前能拒绝或 invalid 其他 writer。
3. write-back 未 sync 时远端不可见，sync 后可见。
4. 故障注入不破坏当前 OBMM/URMA 回归。
5. `gva_direct_demo --mode=mrsw-read-share` 能证明同一 UBA 上两个 read-only reader route 可并存，并在 QEMU 日志中看到两条 `GVA_OWNERSHIP_REGISTER role=reader` 且没有 `GVA_OWNERSHIP_CONFLICT`。
6. `gva_direct_demo --mode=mrsw-conflict` 能证明同一 UBA 上已有 reader 时 writer map 被拒绝，并在 QEMU 日志中看到 `GVA_OWNERSHIP_REGISTER role=reader` 与 `GVA_OWNERSHIP_CONFLICT role=writer existing_role=reader`。
7. `gva_direct_demo --mode=mrsw-writer-conflict` 能证明同一 UBA 上已有 writer 时第二个 writer map 被拒绝，并在 QEMU 日志中看到 `GVA_OWNERSHIP_REGISTER role=writer` 与 `GVA_OWNERSHIP_CONFLICT role=writer existing_role=writer`。

## 10. 验证矩阵

### 10.1 当前可复用环境

当前仓库已有以下入口可复用：

1. QEMU 构建：
   - `guest-linux/aarch64/scripts/build_qemu_binary.sh`
   - 项目约定要求使用该脚本，不直接手工运行 QEMU vendor 目录下的 configure/ninja。
2. guest artifact 构建：
   - `guest-linux/aarch64/scripts/build_guest_artifacts.sh`
   - 支持复用 `out/`、local import、native Linux cross build、remote Linux sync。
3. dual-node 验证：
   - `guest-linux/aarch64/scripts/run_ub_dual_node_demo.sh`
   - `guest-linux/aarch64/scripts/run_ub_dual_node_urma_dataplane_workload_test.sh`
   - `guest-linux/aarch64/scripts/run_ub_dual_node_obmm_import_stress.sh`
4. four/eight-node 验证：
   - `guest-linux/aarch64/scripts/run_ub_four_node_w4_guest.sh`
   - `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh`
   - `guest-linux/aarch64/scripts/run_ub_four_node_obmm_queue_demo.sh`
   - `guest-linux/aarch64/scripts/run_ub_eight_node_obmm_queue_demo.sh`

当前统计输出：

1. `SIM_DEC_STATS` 已由 QEMU `sim_dec_print_global_stats()` 在退出时输出到 QEMU log。
2. dual/four/eight-node 脚本已经按 node 维护 `*_qemu.log` 和 `*_guest.log`。
3. `GVA_STATS` 已由 QEMU 在退出统计中输出，脚本必须检查 GVA 读写计数而不能只依赖 `SIM_DEC_STATS`。
4. Phase A 验收还必须检查 `GVA_S3_MAP` 日志和 guest `/proc/ub_sim_decoder/gva_routes` route dump；guest dump 必须包含 request/effective `p_tag` 和 resolved `mp_table` 字段。

最小测试集：

1. Rust/unit：
   - GVA route overlap
   - map/unmap lifecycle
   - cache policy validation
   - token mismatch
2. guest user-space：
   - `gva_direct_demo --mode=write-read`
   - `gva_direct_demo --mode=unmap-fault`
   - `gva_direct_demo --mode=sync`
   - `gva_direct_demo --mode=mrsw-read-share`
   - `gva_direct_demo --mode=mrsw-conflict`
   - `gva_direct_demo --mode=mrsw-writer-conflict`
3. QEMU dual-node：
   - `run_ub_dual_node_gva_direct_test.sh`
   - `run_ub_dual_node_gva_direct_matrix.sh`
   - existing `run_ub_dual_node_demo.sh`
   - existing `run_ub_dual_node_urma_dataplane_workload_test.sh`
4. QEMU four/eight-node：
   - OBMM pool matrix
   - W4/W5 guest workload smoke path

每个 GVA 功能阶段的验收标准：

1. 不能只看日志推断成功。
2. 必须有 guest 读写数据一致性验证。
3. 必须有 QEMU 统计计数证明命中 GVA/backend 数据面。
4. 必须回归现有 UB Link、URMA、OBMM 路径。

## 11. 对用户体验的影响

对应用和上层 runtime，目标体验是：

1. 仍然通过 `export/import/mmap` 使用远端内存。
2. 不要求应用显式理解 `EID/CNA/p_tag`。
3. 新增 CLI 只用于测试、调试和可观测性。
4. 错误反馈必须指向下一步动作，例如：
   - `route miss: run gva dump-route and check FM convergence`
   - `token mismatch: re-export remote memory and retry import`
   - `cache policy unsupported: retry with --cache=nc`

这符合 GVA 的设计目标：系统承担复杂性，应用看到的是本地化内存语义。

## 12. Phase A 已决策与剩余开放问题

### 12.1 Phase A 已决策

1. `VMID/ASID`：
   - 默认 `0/0`。
   - 测试入口可以显式覆盖。
   - 不从 Linux task/mm 推导。
2. `PTE.offset`：
   - 不进入 guest page table。
   - 作为 GVA metadata 字段。
   - 普通 GVA profile 优先 `User VA == UBA`，此时 `pte_offset=0`。
   - 当普通 GVA 的 User 侧同值 VA 不可用或显式要求 relocation 时，记录 `pte_offset=UBA-User VA`。
   - GSVA profile 由 `GVA Manager` 分配 `gsva_base`，必须满足 `local_va=uba_base=home_va=gsva_base`、`pte_offset=0`。
   - legacy OBMM Phase A 当前未接入 VA translation，默认只记录 `local_va=0`、`pte_offset=0`、`uba_base=remote_uba`。
   - manager-produced map request 必须填写真实 `local_va`，不能使用 legacy `local_va=0` 约定。
3. `p_tag`：
   - Phase A 支持显式非零静态 tag，也支持 `p_tag=0` 由 QEMU backend 按 resolved `mp_table.link_id` 派生 effective tag。
   - Phase B 和 FM link topology 校验 effective tag；显式非零 tag 与 resolved link 不一致时必须失败。
4. `cache_policy`：
   - Phase A 基线只要求 `NC/write-through`。
   - 当前实现已扩展到 Phase D 的 `read-cache`、`write-back` 和 MRSW ownership registry。
5. `obmm_cmd_import`：
   - Phase A 不改 UAPI struct。
   - 使用 `OBMM_SIM_DEC_PRIV_VER_2` 承载 GVA metadata。
   - legacy `OBMM_SIM_DEC_PRIV_VER_1` 继续兼容。
6. `SIM_DEC_OP_MAP`：
   - 不修改现有 v1 payload。
   - 新增 `SIM_DEC_OP_GVA_MAP = 0x07`。
   - QEMU 创建 legacy backend map，同时保存 `SimGvaRouteMeta`。
7. map 基数：
   - Phase A 强制 `GVA route entry : backend map = 1:1`。
   - 不做按页拆分。
   - `map_id` 由 backend/QEMU 分配并在 response 中返回，guest kernel 绑定到 OBMM import registry。
8. 模块放置：
   - Phase A 放在 `ubus/sim` 内。
   - 不新增独立 `ub-gva.ko`。
9. `GVA Manager`：
   - GVA Simulation Phase A 只定义 manager-produced map request 合同。
   - GSVA manager bootstrap、reserved range 协商和 kernel/OBMM aperture registry 在 GSVA 设计中实现。

### 12.2 剩余开放问题

当前设计中的 Phase A/B/C/D 接口决策已经收敛。剩余工作不再是接口选择，而是需要基于新 guest kernel artifact 跑完整两节点、四节点和 TCG-on/off runtime 验证。

## 13. 结论

当前 `ub_sim` 已经具备实现 GVA 仿真的关键底座：OBMM export/import、guest sim decoder service、QEMU `SIM_DEC` map、CPU window、strict DMA hit、UB Link 跨节点 READ/WRITE。

目标架构中不应该存在一个独立的 GVA decoder。真实语义应是：

```text
VA + PTE.offset -> UBA
  -> MMU.S3 ma_table
  -> NoC mp_table
  -> UBC/UB Link
```

因此 `SIM_DEC` 不能被当成 GVA 架构组件。它在当前方案中的角色只是第一阶段复用的 legacy backend：提供已验证的 map storage、CPU window callback、strict DMA fast path 和 UB Link remote read/write。

当前实现不从零写一套 GVA 数据面，也没有把 `SIM_DEC` 提升为架构组件；它复用 legacy backend 承载分阶段模拟：

1. 在现有 backend 上建立显式 `GVA map/MMU.S3 ma_table/NoC mp_table/cache policy` 语义。
2. 通过日志、stats、CLI、fault injection 和双/四/八节点脚本证明 GVA 语义正确。
3. 使用 GVA route metadata side table 作为当前 simulator 的 `PTE.offset` 来源，并通过可选 `SIM_GVA_TCG=1` 把入口前移到 QEMU ARM MMU/TCG translation hook。
4. 若后续需要更贴近硬件 PTE encoding，再把 side table 替换为 guest PTE decoder；GVA manager、ma_table/mp_table、fault/stats/test contract 保持不变。

这样能最小化改动风险，同时把 PPT 中的 `PTE.offset`、`MMU.S3 ma_table`、`NoC mp_table`、`UBC/UMMU validation` 转化为可测试、可回归、可观测的工程对象，并保留最终摆脱 `SIM_DEC` 对外概念的架构方向。
