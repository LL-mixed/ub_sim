# GVA/GSVA 两节点与四节点运行验证报告

日期：2026-06-05
仓库：`/Volumes/repos/ub_sim`
依据：`guest-linux/aarch64/logs` 下已完成的两节点和四节点 QEMU 运行日志

## 1. 结论

基于本报告引用的运行日志，当前 `ub_sim` 中的 GVA 与 GSVA 模拟已经在两类拓扑下完成端到端验证：

1. 两节点拓扑下，GSVA manager bootstrap、GSVA identity 模式和 GVA direct write-read 模式均完成。
2. 四节点 full-mesh 拓扑下，GSVA manager bootstrap 与 GSVA matrix 模式均完成。
3. QEMU 侧可以观测到 `GVA_S3_MAP`、`GVA_ROUTE_DUMP`、`GVA_PATH`、`SIM_DEC_STATS`、`GVA_STATS`，说明访问不是仅停留在 guest 应用层，而是进入了当前 QEMU/SIM_DEC 承载的 GVA/S3 路由模拟路径。
4. GSVA identity 与 GSVA matrix 模式中，`pte_offset=0`、`local_va == home_va == uba` 成立，符合当前 GSVA 设计目标。
5. GVA direct 模式中，`local_va != home_va != uba` 且 `pte_offset != 0`，可以证明普通 GVA 的非 identity 地址映射路径也可用。
6. 四节点冲突注入场景能触发 `aperture reserve failed errno=17` 与 `result=fail`，说明 GSVA aperture reservation 的失败路径可观测，且不会被误判为成功。

整体判断：这些日志足以证明当前实现中的 GVA/GSVA 控制面注册、QEMU 路由建立、跨节点访问路径和数据一致性在两节点与四节点场景下工作正确。

## 2. 验证对象

本报告覆盖以下能力：

| 能力 | 验证目标 | 通过标准 |
| --- | --- | --- |
| GSVA manager bootstrap | 多 OS 上的 GVA manager 通过 OBMM 队列完成 bootstrap，并将 aperture 注册给 guest kernel/OBMM | 每节点出现 `obmm bootstrap -> ok`、`manager queues -> ok`、`bootstrap hello -> ok`、`kernel aperture registry -> ok`、`result=done registry=kernel-obmm` |
| GSVA identity | `user_va == uba == home_va` 的 global shared virtual address 成立 | home fixed export 成功，peer 建立 `address_profile=2`、`pte_offset=0` 的 GVA/S3 route，peer 读写数据一致 |
| GVA direct | 非 identity 的 GVA 映射成立 | peer 建立 `address_profile=1`、`pte_offset != 0` 的 GVA/S3 route，peer 读写数据一致 |
| GSVA matrix | 四节点每个节点都能映射其他节点 GSVA slice | 每节点至少 3 条 `GVA_S3_MAP address_profile=2 pte_offset=0`，并完成 matrix 数据读写 |
| aperture 冲突 | GSVA reserved address range 冲突能被检测 | 冲突节点出现 `aperture reserve failed errno=17`，manager 报 `result=fail` |

## 3. 使用的运行日志

| 场景 | 日志目录 |
| --- | --- |
| 2-node GSVA manager bootstrap | `guest-linux/aarch64/logs/2026-06-05_13-49-52_gsva_mgr_4165` |
| 2-node GSVA identity demo | `guest-linux/aarch64/logs/2026-06-05_13-50-05_gsva_demo_31826` |
| 2-node GVA direct demo | `guest-linux/aarch64/logs/2026-06-05_13-50-15_gva_direct_32219` |
| 4-node GSVA manager bootstrap | `guest-linux/aarch64/logs/2026-06-05_10-12-55_gsva_mgr4_5846` |
| 4-node GSVA matrix demo | `guest-linux/aarch64/logs/2026-06-05_10-20-01_gsva_matrix4_25263` |
| 4-node GSVA manager conflict | `guest-linux/aarch64/logs/2026-06-05_10-10-04_gsva_mgr4_29378` |

## 4. 两节点 GSVA manager bootstrap

运行目录：`guest-linux/aarch64/logs/2026-06-05_13-49-52_gsva_mgr_4165`

### 4.1 NodeA 证据

`nodeA_guest.log`：

```text
2151:[gva_manager] obmm bootstrap -> ok count=2
2160:[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410001
2161:[gva_manager] result=done generation=0x475356410001 aperture_base=0x700000000000 aperture_size=0x1000000 registry=kernel-obmm
```

解读：

1. `obmm bootstrap -> ok count=2` 说明 NodeA 已经进入 2 节点 OBMM bootstrap，并完成本节点初始化。
2. `kernel aperture registry -> ok` 说明协商出的 GSVA aperture 已经注册进 guest kernel/OBMM 感知的地址管理路径，而不是只停留在 userspace manager 本地状态。
3. `registry=kernel-obmm` 是最终完成态，表示 manager 认为 kernel 与 OBMM 两侧都已接受该 aperture。

### 4.2 NodeB 证据

`nodeB_guest.log`：

```text
2149:[gva_manager] obmm bootstrap -> ok count=2
2154:[gva_manager] manager queues -> ok
2155:[gva_manager] bootstrap hello -> ok peers=1
2156:[gva_manager] aperture reserved registry=process-local base=0x700000000000 size=0x1000000
2158:[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410001
2159:[gva_manager] result=done generation=0x475356410001 aperture_base=0x700000000000 aperture_size=0x1000000 registry=kernel-obmm
```

解读：

1. `manager queues -> ok` 说明 GSVA manager 间通信依赖的 OBMM MPMC queue 已建立。
2. `bootstrap hello -> ok peers=1` 说明 NodeB 看到了除自己外的 1 个 peer，符合两节点拓扑预期。
3. NodeB 的 `base/size/generation` 与 NodeA 一致，说明双方没有各自 reserve 不同地址段，而是达成同一个 global aperture。

### 4.3 判定

两节点均完成 OBMM bootstrap，manager MPMC queue 可用，双方达成同一 GSVA aperture：

```text
base=0x700000000000
size=0x1000000
generation=0x475356410001
registry=kernel-obmm
```

这证明 GSVA 地址管理组件能够在两个 OS 上完成 bootstrap，并将 reserved aperture 注册到 guest kernel/OBMM 感知的地址管理路径中。

## 5. 两节点 GSVA identity demo

运行目录：`guest-linux/aarch64/logs/2026-06-05_13-50-05_gsva_demo_31826`

### 5.1 Guest 层证据

Home 节点 `nodeA_guest.log`：

```text
2145:[obmm_gsva_demo] kernel aperture registry -> ok base=0x700000000000 size=0x400000
2148:[obmm_gsva_demo] fixed export -> ok mem_id=0x1 uba=0x700000000000 token=96
2151:[obmm_gsva_demo] home wrote value=0x1111222233334444 ptr=0x700000000000
2152:[obmm_gsva_demo] result=done mode=identity role=home ptr=0x700000000000 home_va=0x700000000000 uba=0x700000000000 value=0xaaaabbbbccccdddd
```

解读：

1. `fixed export -> ok ... uba=0x700000000000` 说明 home 节点没有让 OBMM 随机分配 UBA，而是按 GSVA 设计要求固定导出到 global virtual address。
2. `home wrote value=... ptr=0x700000000000` 说明 home 侧实际在该 GSVA 指针上写入初始 payload。
3. `result=done ... home_va=... uba=...` 同时打印 `ptr/home_va/uba`，用于证明 home 侧地址三元组相等。
4. 结果值变成 `0xaaaabbbbccccdddd`，说明 peer 已经访问同一 payload 并回写成功。

Peer 节点 `nodeB_guest.log`：

```text
2145:[obmm_gsva_demo] kernel aperture registry -> ok base=0x700000000000 size=0x400000
2153:[    6.588744][  T156] UB SIM Decoder: gva map created id=1 pa=60000000000 size=400000 remote_uba=700000000000 token=96 vmid=0 asid=0 local_va=700000000000 home_va=700000000000 pte_offset=0 address_profile=2
2155:[obmm_gsva_demo] result=done mode=identity role=peer ptr=0x700000000000 user_va=0x700000000000 uba=0x700000000000 value=0xaaaabbbbccccdddd
```

解读：

1. `gva map created` 是 guest kernel/OBMM 向 SIM_DEC/QEMU 下发 GVA route 的证据。
2. `address_profile=2` 表示该 route 使用 GSVA identity profile。
3. `local_va/home_va/remote_uba` 全部为 `0x700000000000`，且 `pte_offset=0`，这正是 `user_va == uba == home_va` 的关键条件。
4. peer 的 `result=done` 打印 `user_va` 和 `uba`，证明应用看到的用户态地址与 OBMM UBA 等值。

关键判断：

```text
user_va=0x700000000000
uba=0x700000000000
home_va=0x700000000000
pte_offset=0
address_profile=2
```

这正是 GSVA identity 目标，即 `user_va == uba == home_va`。

### 5.2 QEMU/S3 路由证据

Peer 节点 `nodeB_qemu.log`：

```text
1714:GVA_S3_MAP id=1 gva_id=1 vmid=0 asid=0 local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 pa=60000000000 size=400000 dcna=50370 tid=0 token=96 upi=0 p_tag=0 ubc_port=1 lane=1 link_id=65537 map_source=2 address_profile=2 cache_policy=1
1715:GVA_ROUTE_DUMP state=active map_id=1 gva_id=1 vmid=0 asid=0 local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 pa=60000000000 size=400000 ma_table.dcna=50370 ma_table.tid=0 ma_table.token=96 ma_table.upi=0 mp_table.p_tag=0 mp_table.ubc_port=1 mp_table.lane=1 mp_table.link_id=65537 map_source=2 address_profile=2 cache_policy=1 access_flags=0
1721:GVA_PATH gva_path=cpu_window op=read map_id=1 gva_id=1 local_va=700000000000 offset=8 remote_uba=700000000008 size=8 count=1 address_profile=2
1729:GVA_PATH gva_path=cpu_window op=write map_id=1 gva_id=1 local_va=700000000000 offset=10 remote_uba=700000000010 size=8 count=1 address_profile=2
1734:SIM_DEC_STATS gva_cpu_reads=4 gva_cpu_writes=3 gva_cpu_rbytes=32 gva_cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
1735:GVA_STATS cpu_reads=4 cpu_writes=3 cpu_rbytes=32 cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
```

解读：

1. `GVA_S3_MAP` 是 QEMU 侧真正安装 S3/GVA route 的日志。它把 `local_va/home_va/uba` 全部记录为 `700000000000`，并带有 `address_profile=2`、`pte_offset=0`，证明 QEMU 看到的是 GSVA identity route。
2. `GVA_ROUTE_DUMP state=active` 说明该 route 不是只被解析过，而是处于 active 状态。`ma_table.*` 与 `mp_table.*` 字段说明 route 已经包含 MA/MP 两类元数据，包括 `dcna/tid/token/upi/p_tag/link_id`。
3. `GVA_PATH ... op=read` 和 `op=write` 说明 CPU window 访问命中了该 GVA route。`remote_uba=700000000008` 等于 `uba + offset`，说明地址转换按 route 元数据执行。
4. `SIM_DEC_STATS` 与 `GVA_STATS` 的 read/write 计数非零，证明测试过程中确实发生了 GVA 读写，而不是只完成 map。
5. `read_timeouts=0 read_errors=0 write_errors=0` 是正确性边界：没有超时、读错误或写错误，访问路径完成。

### 5.3 判定

该场景证明：

1. home 节点能用 fixed UBA export 出 `0x700000000000`。
2. peer 节点以相同 virtual address 导入并访问同一对象。
3. QEMU 中已经建立 `GVA_S3_MAP`，不是 guest 应用层的本地假成功。
4. GVA 统计显示 peer 侧产生了实际 GVA CPU read/write，且 `read_errors=0`、`write_errors=0`。

因此，两节点 GSVA identity 模式工作正确。

## 6. 两节点 GVA direct demo

运行目录：`guest-linux/aarch64/logs/2026-06-05_13-50-15_gva_direct_32219`

### 6.1 Guest 层证据

Home 节点 `nodeA_guest.log`：

```text
2147:[gva_direct_demo] home wrote value=0x13579bdf2468ace0 home_va=0x720000000000 uba=0xffffffc00000
2148:[gva_direct_demo] result=done mode=write-read role=home local_va=0x710000000000 home_va=0x720000000000 uba=0xffffffc00000 pte_offset=remote-local value=0xfdb97531eca86420 sync_done=0
```

解读：

1. `home_va=0x720000000000` 是 home 侧实际 mmap/export 的虚拟地址。
2. `uba=0xffffffc00000` 与 `home_va` 不相等，说明该场景不是 GSVA identity，而是普通 GVA。
3. 最终 `value=0xfdb97531eca86420` 表示 peer 已经通过 GVA route 访问并回写 home payload。

Peer 节点 `nodeB_guest.log`：

```text
2151:[    6.595590][  T156] UB SIM Decoder: gva map created id=1 pa=60000000000 size=400000 remote_uba=ffffffc00000 token=96 vmid=0 asid=0 local_va=710000000000 home_va=720000000000 pte_offset=8effffc00000 address_profile=1
2154:[gva_direct_demo] result=done mode=write-read role=peer local_va=0x710000000000 home_va=0x720000000000 uba=0xffffffc00000 pte_offset=0x8effffc00000 value=0xfdb97531eca86420
```

解读：

1. `address_profile=1` 表示 generic GVA，而非 GSVA identity。
2. `local_va=0x710000000000` 是 peer 用户态访问地址，`home_va=0x720000000000` 是 home 侧虚拟地址，`uba=0xffffffc00000` 是 OBMM/remote backing 地址，三者不相等。
3. `pte_offset=0x8effffc00000` 是 `uba - local_va`，说明 QEMU 路由需要通过 offset 做地址转换。
4. peer 侧最终读写值一致，说明非 identity GVA 的 offset 映射路径可用。

关键判断：

```text
local_va=0x710000000000
home_va=0x720000000000
uba=0xffffffc00000
pte_offset=0x8effffc00000
address_profile=1
```

这证明普通 GVA direct 并非 GSVA identity，而是使用非零 `pte_offset` 的地址转换。

### 6.2 QEMU/S3 路由证据

Peer 节点 `nodeB_qemu.log`：

```text
1709:SIM_DEC: GVA_MAP success id=1 pa=60000000000 sz=400000 remote_uba=ffffffc00000 token=96 map_source=2 address_profile=1 local_va=710000000000 home_va=720000000000 pte_offset=8effffc00000 vmid=0 asid=0 p_tag=0 gva_id=1
1711:GVA_ROUTE_DUMP state=active map_id=1 gva_id=1 vmid=0 asid=0 local_va=710000000000 home_va=720000000000 pte_offset=8effffc00000 uba=ffffffc00000 pa=60000000000 size=400000 ma_table.dcna=50370 ma_table.tid=0 ma_table.token=96 ma_table.upi=0 mp_table.p_tag=0 mp_table.ubc_port=1 mp_table.lane=1 mp_table.link_id=65537 map_source=2 address_profile=1 cache_policy=1 access_flags=0
1717:GVA_PATH gva_path=cpu_window op=read map_id=1 gva_id=1 local_va=710000000000 offset=8 remote_uba=ffffffc00008 size=8 count=1 address_profile=1
1725:GVA_PATH gva_path=cpu_window op=write map_id=1 gva_id=1 local_va=710000000000 offset=10 remote_uba=ffffffc00010 size=8 count=1 address_profile=1
1730:SIM_DEC_STATS gva_cpu_reads=4 gva_cpu_writes=3 gva_cpu_rbytes=32 gva_cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
1731:GVA_STATS cpu_reads=4 cpu_writes=3 cpu_rbytes=32 cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
```

解读：

1. `SIM_DEC: GVA_MAP success` 是 QEMU 接受并创建 GVA map 的入口日志，证明 guest 下发的 GVA metadata 被 QEMU 控制面接收。
2. `address_profile=1`、`local_va=710000000000`、`home_va=720000000000`、`pte_offset=8effffc00000` 共同证明这是 generic GVA route，不是 GSVA route。
3. `GVA_ROUTE_DUMP state=active` 说明 route 已安装并可查；`ma_table` 与 `mp_table` 字段显示 token、UPI、p_tag、端口/lane/link 等路由元数据均在 QEMU 中形成。
4. `GVA_PATH ... remote_uba=ffffffc00008` 等于 `uba + offset`，证明 peer 对 `local_va + offset` 的 CPU window 访问被转换到 remote UBA。
5. `gva_cpu_reads=4 gva_cpu_writes=3` 和 `GVA_STATS cpu_reads=4 cpu_writes=3` 相互印证，说明读写都经过 GVA 路径；错误计数为 0，说明访问完成且无故障。

### 6.3 判定

该场景证明：

1. GVA direct 可以创建非 identity GVA route。
2. `pte_offset` 被写入 QEMU/S3 路由元数据。
3. peer 访问经由 `GVA_PATH gva_path=cpu_window` 转换到 remote UBA。
4. GVA read/write 统计非零，且没有 read/write error。

因此，两节点普通 GVA 映射工作正确。

## 7. 四节点 GSVA manager bootstrap

运行目录：`guest-linux/aarch64/logs/2026-06-05_10-12-55_gsva_mgr4_5846`

### 7.1 每节点 bootstrap 证据

NodeA `nodeA_guest.log`：

```text
2190:[gva_manager] obmm bootstrap -> ok count=4
2203:[gva_manager] manager queues -> ok
2204:[gva_manager] bootstrap hello -> ok peers=3
2205:[gva_manager] aperture reserved registry=process-local base=0x700000000000 size=0x1000000
2207:[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410004
2208:[gva_manager] result=done generation=0x475356410004 aperture_base=0x700000000000 aperture_size=0x1000000 registry=kernel-obmm
```

解读：

1. `count=4` 和 `peers=3` 同时出现，说明 NodeA 是按四节点 full-mesh 预期进入 bootstrap，并识别到除自己外的三个 manager。
2. `aperture reserved` 与 `kernel aperture registry` 都使用 `0x700000000000/0x1000000`，说明 userspace manager reserve 与 kernel/OBMM registry 使用同一地址区间。
3. `registry=kernel-obmm` 表明该节点已经从“本地 reserve”推进到“kernel/OBMM 均可见”的完成态。

NodeB `nodeB_guest.log`：

```text
2176:[gva_manager] obmm bootstrap -> ok count=4
2189:[gva_manager] manager queues -> ok
2190:[gva_manager] bootstrap hello -> ok peers=3
2191:[gva_manager] aperture reserved registry=process-local base=0x700000000000 size=0x1000000
2193:[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410004
2194:[gva_manager] result=done generation=0x475356410004 aperture_base=0x700000000000 aperture_size=0x1000000 registry=kernel-obmm
```

解读：

NodeB 与 NodeA 的 `base/size/generation` 完全一致，且同样看到 `peers=3`。这证明四节点不是多个局部两节点连接拼出来的结果，而是所有节点都参与同一个 generation 的 GSVA aperture 协商。

NodeC `nodeC_guest.log`：

```text
2268:[gva_manager] obmm bootstrap -> ok count=4
2281:[gva_manager] manager queues -> ok
2282:[gva_manager] bootstrap hello -> ok peers=3
2283:[gva_manager] aperture reserved registry=process-local base=0x700000000000 size=0x1000000
2285:[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410004
2286:[gva_manager] result=done generation=0x475356410004 aperture_base=0x700000000000 aperture_size=0x1000000 registry=kernel-obmm
```

解读：

NodeC 的完成日志用于排除“只有先启动的节点成功”的可能性。它同样在四节点 topology 下看到三个 peer，并完成同一 aperture 的 kernel/OBMM 注册。

NodeD 同样完成 `obmm bootstrap`、`manager queues`、`bootstrap hello peers=3`、`kernel aperture registry` 和 `result=done registry=kernel-obmm`。

### 7.2 判定

四个节点全部达成：

```text
node_count=4
peers=3
aperture_base=0x700000000000
aperture_size=0x1000000
generation=0x475356410004
registry=kernel-obmm
```

这证明四节点 full-mesh 下，GSVA manager 的 OBMM MPMC bootstrap 和 aperture 注册在所有 OS 上一致完成。

## 8. 四节点 GSVA matrix demo

运行目录：`guest-linux/aarch64/logs/2026-06-05_10-20-01_gsva_matrix4_25263`

### 8.1 Guest 层完成证据

每节点结果：

```text
nodeA_guest.log:2205:[obmm_gsva_demo] result=done mode=matrix node=0 node_count=4 slice_base=0x700000000000 ptr=0x700000000000 value_from_node0=0x4753564d00000000 value_from_last=0x4753564d00000300
nodeB_guest.log:2191:[obmm_gsva_demo] result=done mode=matrix node=1 node_count=4 slice_base=0x700000400000 ptr=0x700000400000 value_from_node0=0x4753564d00000001 value_from_last=0x4753564d00000301
nodeC_guest.log:2283:[obmm_gsva_demo] result=done mode=matrix node=2 node_count=4 slice_base=0x700000800000 ptr=0x700000800000 value_from_node0=0x4753564d00000002 value_from_last=0x4753564d00000302
nodeD_guest.log:2281:[obmm_gsva_demo] result=done mode=matrix node=3 node_count=4 slice_base=0x700000c00000 ptr=0x700000c00000 value_from_node0=0x4753564d00000003 value_from_last=0x4753564d00000303
```

解读：

1. 每个节点的 `ptr` 都等于自己的 `slice_base`，说明本地 userspace 看到的地址就是 GSVA 分配的 global slice 地址。
2. `node_count=4` 说明 demo 以四节点矩阵模式运行，而不是复用两节点身份映射。
3. `value_from_node0` 和 `value_from_last` 是 matrix payload 的跨节点写入结果。每个节点都读到来自 node0 和最后一个节点 node3 的写入，说明至少覆盖了首尾两个远端 writer；结合源码中的全 writer 校验逻辑，可证明四节点全写入矩阵完成。

四个 slice 分布：

| 节点 | slice_base | ptr |
| --- | --- | --- |
| NodeA | `0x700000000000` | `0x700000000000` |
| NodeB | `0x700000400000` | `0x700000400000` |
| NodeC | `0x700000800000` | `0x700000800000` |
| NodeD | `0x700000c00000` | `0x700000c00000` |

### 8.2 QEMU/S3 路由证据

NodeA 需要访问 NodeB、NodeC、NodeD 三个 remote slice。`nodeA_qemu.log` 显示三条 `GVA_S3_MAP`：

```text
2186:GVA_S3_MAP id=1 ... local_va=700000400000 home_va=700000400000 pte_offset=0 uba=700000400000 ... address_profile=2
2201:GVA_S3_MAP id=2 ... local_va=700000800000 home_va=700000800000 pte_offset=0 uba=700000800000 ... address_profile=2
2212:GVA_S3_MAP id=3 ... local_va=700000c00000 home_va=700000c00000 pte_offset=0 uba=700000c00000 ... address_profile=2
```

解读：

NodeA 的本地 slice 是 `0x700000000000`，所以它需要导入 NodeB、NodeC、NodeD 三个 remote slice。三条 `GVA_S3_MAP` 分别覆盖 `0x700000400000`、`0x700000800000`、`0x700000c00000`，数量等于 `node_count - 1`。每条 route 都是 `local_va == home_va == uba`、`pte_offset=0`、`address_profile=2`，说明 NodeA 对三个 remote slice 使用 GSVA identity 语义。

NodeB 需要访问 NodeA、NodeC、NodeD 三个 remote slice。`nodeB_qemu.log` 显示三条 `GVA_S3_MAP`：

```text
2158:GVA_S3_MAP id=1 ... local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 ... address_profile=2
2166:GVA_S3_MAP id=2 ... local_va=700000800000 home_va=700000800000 pte_offset=0 uba=700000800000 ... address_profile=2
2174:GVA_S3_MAP id=3 ... local_va=700000c00000 home_va=700000c00000 pte_offset=0 uba=700000c00000 ... address_profile=2
```

解读：

NodeB 的本地 slice 是 `0x700000400000`，因此 QEMU 中应只出现其他三个节点的 GSVA route：NodeA、NodeC、NodeD。日志正好对应这三个地址，证明 matrix import 没有漏掉远端节点，也没有错误地把本地 slice 当成 remote route。

NodeC 需要访问 NodeA、NodeB、NodeD 三个 remote slice。`nodeC_qemu.log` 显示三条 `GVA_S3_MAP`：

```text
2458:GVA_S3_MAP id=1 ... local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 ... address_profile=2
2472:GVA_S3_MAP id=2 ... local_va=700000400000 home_va=700000400000 pte_offset=0 uba=700000400000 ... address_profile=2
2480:GVA_S3_MAP id=3 ... local_va=700000c00000 home_va=700000c00000 pte_offset=0 uba=700000c00000 ... address_profile=2
```

解读：

NodeC 的本地 slice 是 `0x700000800000`，日志中的三条 route 覆盖 NodeA、NodeB、NodeD。再次验证每节点建立的 route 集合是“所有远端节点”，不是固定从 NodeA 出发的单向测试。

NodeD 同样建立三条 remote GSVA route，分别覆盖 NodeA、NodeB、NodeC 的 slice。

### 8.3 访问路径与统计证据

NodeA `nodeA_qemu.log`：

```text
2222:GVA_PATH gva_path=cpu_window op=read map_id=1 gva_id=2 local_va=700000400000 offset=8 remote_uba=700000400008 size=8 count=1 address_profile=2
2245:GVA_PATH gva_path=cpu_window op=write map_id=1 gva_id=2 local_va=700000400000 offset=28 remote_uba=700000400028 size=8 count=1 address_profile=2
2307:SIM_DEC_STATS gva_cpu_reads=43 gva_cpu_writes=3 gva_cpu_rbytes=344 gva_cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
2308:GVA_STATS cpu_reads=43 cpu_writes=3 cpu_rbytes=344 cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
```

解读：

1. NodeA 对 `local_va=700000400000` 的 read/write 命中 `map_id=1`，该地址是 NodeB slice，不是 NodeA 本地 slice。
2. `remote_uba=700000400008` 和 `remote_uba=700000400028` 等于 `local_va + offset`，说明 GSVA identity 下 remote UBA 与 local VA 保持等值偏移。
3. `gva_cpu_reads=43`、`gva_cpu_writes=3` 表示 NodeA 在 matrix 校验中发生了多次远端读取和写入。
4. 错误计数为 0，说明这些远端访问均成功完成。

NodeB `nodeB_qemu.log`：

```text
2181:GVA_PATH gva_path=cpu_window op=read map_id=1 gva_id=1 local_va=700000000000 offset=8 remote_uba=700000000008 size=8 count=1 address_profile=2
2201:GVA_PATH gva_path=cpu_window op=write map_id=1 gva_id=1 local_va=700000000000 offset=30 remote_uba=700000000030 size=8 count=1 address_profile=2
2327:SIM_DEC_STATS gva_cpu_reads=72 gva_cpu_writes=3 gva_cpu_rbytes=576 gva_cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
2328:GVA_STATS cpu_reads=72 cpu_writes=3 cpu_rbytes=576 cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
```

解读：

1. NodeB 访问的 `local_va=700000000000` 是 NodeA slice，证明 NodeB 能反向访问 NodeA，而不是只有 NodeA 能访问其他节点。
2. `count=1` 到后续更高计数说明 QEMU 在同一 route 上持续记录访问，不是一次性初始化日志。
3. `72` 次 GVA read 高于 NodeA/NodeC，是 demo 等待与轮询时序的结果；关键点不是精确数值一致，而是 read/write 非零且错误为 0。

NodeC `nodeC_qemu.log`：

```text
2487:GVA_PATH gva_path=cpu_window op=read map_id=1 gva_id=1 local_va=700000000000 offset=8 remote_uba=700000000008 size=8 count=1 address_profile=2
2507:GVA_PATH gva_path=cpu_window op=write map_id=1 gva_id=1 local_va=700000000000 offset=38 remote_uba=700000000038 size=8 count=1 address_profile=2
2579:SIM_DEC_STATS gva_cpu_reads=45 gva_cpu_writes=3 gva_cpu_rbytes=360 gva_cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
2580:GVA_STATS cpu_reads=45 cpu_writes=3 cpu_rbytes=360 cpu_wbytes=24 ... read_timeouts=0 read_errors=0 write_errors=0
```

解读：

1. NodeC 对 `local_va=700000000000` 的访问同样命中 NodeA slice，证明第三个节点也能通过 GSVA route 访问同一 global address。
2. `op=read` 与 `op=write` 同时存在，说明 matrix 不是只读共享，而是包含远端写入。
3. `SIM_DEC_STATS` 和 `GVA_STATS` 的计数一致，说明 SIM_DEC 层和 GVA 统计层对这批访问的观测一致。

### 8.4 判定

四节点 matrix 模式证明了：

1. 每个节点都保留自己的 GSVA slice 指针，`ptr == slice_base`。
2. 每个节点都能导入其他三个节点的 GSVA slice。
3. 每条 route 都是 `address_profile=2`、`pte_offset=0`，满足 GSVA identity 语义。
4. 每个节点都有 GVA read/write 统计，且没有 read timeout/read error/write error。
5. `value_from_node0` 和 `value_from_last` 在四个节点上均按 matrix 规则出现，说明跨节点写入与读取结果一致。

因此，四节点 GSVA matrix 模式工作正确。

## 9. 四节点冲突注入验证

运行目录：`guest-linux/aarch64/logs/2026-06-05_10-10-04_gsva_mgr4_29378`

冲突节点 NodeC `nodeC_guest.log`：

```text
2268:[gva_manager] obmm bootstrap -> ok count=4
2281:[gva_manager] manager queues -> ok
2283:[gva_manager] bootstrap hello -> ok peers=3
2284:[gva_manager] aperture reserve failed base=0x700000000000 size=0x1000000 errno=17
2285:[gva_manager] result=fail generation=0x475356410004
```

解读：

1. NodeC 已经完成 OBMM bootstrap 和 peer discovery，因此失败不是因为节点没启动或 manager queue 不可用。
2. `aperture reserve failed ... errno=17` 明确说明失败点在 GSVA aperture reserve，`17` 对应 `EEXIST`，即目标地址区间已经被占用。
3. `result=fail` 说明 manager 没有在冲突后继续注册 kernel/OBMM aperture，符合 reserved address range 必须唯一的设计要求。

其他节点能看到 peer failure。NodeA `nodeA_guest.log`：

```text
2209:[gva_manager] peer=2 reported bootstrap error status=17
2210:[gva_manager] result=fail generation=0x475356410004
```

解读：

1. `peer=2` 对应发生冲突的 NodeC，说明错误不是局部静默失败，而是通过 manager bootstrap 协议传播给其他节点。
2. NodeA 也进入 `result=fail`，说明任一节点 aperture reserve 失败会导致该 generation 的全局 bootstrap 失败，避免形成部分节点成功、部分节点失败的不一致 GSVA aperture。

判定：

1. 冲突路径不是静默成功。
2. 失败原因是 aperture reservation 冲突，`errno=17` 即 `EEXIST`。
3. manager 能传播 peer bootstrap error 并整体失败。

这证明 GSVA address reservation 的保护路径可用。

## 10. Demo 逻辑与代码佐证

本节补充 demo 代码路径和关键逻辑，说明前面日志不是孤立输出，而是由明确的 guest app 流程触发：guest initramfs 根据 kernel command line 启动 demo，demo 通过 OBMM export/import 与 GVA metadata 参数驱动 QEMU/SIM_DEC 建立 route，最后通过共享 payload 读写完成数据校验。

### 10.1 initramfs demo 入口

入口文件：`guest-linux/aarch64/initramfs/run_demo`

关键逻辑：

```sh
run_obmm_gsva_demo() {
  local args=""
  local token=""

  read_cmdline
  for token in $cmdline; do
    case "$token" in
      obmm_gsva_mode=*)
        args="$args --mode ${token#*=}"
        ;;
      obmm_gsva_base=*)
        args="$args --base ${token#*=}"
        ;;
      obmm_gsva_size=*)
        args="$args --size ${token#*=}"
        ;;
      obmm_gsva_node_count=*)
        args="$args --node-count ${token#*=}"
        ;;
    esac
  done

  run_binary "linqu_ub_obmm_gsva_demo" /bin/linqu_ub_obmm_gsva_demo $args
}

run_gva_direct_demo() {
  local args=""
  local token=""

  read_cmdline
  for token in $cmdline; do
    case "$token" in
      gva_direct_mode=*)
        args="$args --mode ${token#*=}"
        ;;
      gva_direct_size=*)
        args="$args --size ${token#*=}"
        ;;
      gva_direct_local_va=*)
        args="$args --local-va ${token#*=}"
        ;;
      gva_direct_home_va=*)
        args="$args --home-va ${token#*=}"
        ;;
    esac
  done

  run_binary "linqu_gva_direct_demo" /bin/linqu_gva_direct_demo $args
}
```

这说明测试脚本中的 kernel append 参数不是注释性配置，而是直接进入 guest userspace demo：

```text
obmm_gsva_demo -> /bin/linqu_ub_obmm_gsva_demo --mode ... --base ... --size ... --node-count ...
gva_direct_demo -> /bin/linqu_gva_direct_demo --mode ... --local-va ... --home-va ... --size ...
```

### 10.2 Demo 实现逻辑总览

当前报告里的 demo 不是单个程序一次性完成所有验证，而是由三个 guest userspace 程序分别覆盖不同层次：

| Demo | 源码 | 主要验证层 | 核心动作 | 成功条件 |
| --- | --- | --- | --- | --- |
| `gva_manager_bootstrap` | `guest-linux/aarch64/apps/gva_manager/gva_manager.c` | GSVA 全局地址管理面 | 通过 OBMM MPMC queue 交换 HELLO/PROPOSE/ACCEPT/COMMIT，reserve aperture，并向 OBMM 注册 aperture | 每节点 `registry=kernel-obmm`，同一 `base/size/generation` |
| `obmm_gsva_demo --mode identity` | `guest-linux/aarch64/apps/obmm_gsva_demo/obmm_gsva_demo.c` | 两节点 GSVA identity 数据面 | home fixed UBA export；peer 用 `GSVA_IDENTITY` profile import；双方在同一 VA 上读写 payload | `user_va == uba == home_va`，`pte_offset=0`，peer 写回值被 home 读到 |
| `obmm_gsva_demo --mode matrix` | `guest-linux/aarch64/apps/obmm_gsva_demo/obmm_gsva_demo.c` | 四节点 GSVA full-mesh 数据面 | 每节点 export 自己 slice，并 import 其他 `node_count - 1` 个 slice；所有 writer 写入所有 owner payload | 每节点有 3 条 GSVA route，`value_from_node0/value_from_last` 正确 |
| `gva_direct_demo --mode write-read` | `guest-linux/aarch64/apps/gva_direct_demo/gva_direct_demo.c` | 普通 GVA offset 映射数据面 | home 普通 export；peer 计算 `pte_offset = remote_uba - local_va` 并用 `GENERIC_GVA` profile import | `address_profile=1`，`pte_offset != 0`，peer/home 双向读写成功 |

整体调用链如下：

```text
QEMU script
  -> kernel append: gva_manager_bootstrap / obmm_gsva_demo / gva_direct_demo
  -> initramfs /bin/run_demo
  -> /bin/linqu_gva_manager 或 /bin/linqu_ub_obmm_gsva_demo 或 /bin/linqu_gva_direct_demo
  -> /dev/obmm ioctl + OBMM helper
  -> guest kernel OBMM / sim-decoder
  -> QEMU SIM_DEC GVA_MAP / GVA_S3_MAP / GVA_PATH
```

从验证角度看，demo 分成三层断言：

1. **控制面断言**：manager 或 demo 必须先把 GSVA aperture 注册到 OBMM/kernel，日志表现为 `kernel aperture registry -> ok`。
2. **路由断言**：import 必须下发到 QEMU 并形成 `GVA_S3_MAP` / `GVA_ROUTE_DUMP state=active`。
3. **数据面断言**：guest payload 的 phase/value 必须被远端节点读写改变，并且 QEMU 侧出现 `GVA_PATH` 和非零 `GVA_STATS`。

### 10.3 GVA manager bootstrap 实现逻辑

源码：`guest-linux/aarch64/apps/gva_manager/gva_manager.c`

`gva_manager` 负责把“GSVA address range 是哪一段”从脚本参数落实到每个 OS 的本地地址空间和 OBMM/kernel registry 中。它不是数据面 demo；它验证的是 global address manager 的 bootstrap 管理面。

入口参数来自 `run_demo`：

```sh
run_gva_manager_bootstrap() {
  local args="--bootstrap"

  read_cmdline
  for token in $cmdline; do
    case "$token" in
      gva_manager_node_id=*)
        args="$args --node-id ${token#*=}"
        ;;
      gva_manager_node_count=*)
        args="$args --node-count ${token#*=}"
        ;;
      gva_manager_generation=*)
        args="$args --generation ${token#*=}"
        ;;
      gva_manager_aperture_base=*)
        args="$args --aperture-base ${token#*=}"
        ;;
      gva_manager_aperture_size=*)
        args="$args --aperture-size ${token#*=}"
        ;;
      gva_manager_conflict=1)
        args="$args --conflict"
        ;;
    esac
  done

  run_binary "linqu_gva_manager" /bin/linqu_gva_manager $args
}
```

核心实现流程：

1. 每个节点先通过 OBMM MPMC queue 发送并接收 `HELLO`，确认同一 generation 中的 peer 数。
2. Node0 作为 proposer，在本进程中用 `MAP_FIXED_NOREPLACE` reserve 目标 aperture，防止该地址段被普通 mmap 占用。
3. Node0 调 `OBMM_CMD_GSVA_APERTURE_REGISTER`，把 aperture 注册到 guest kernel/OBMM。
4. Node0 向其他节点发送 `APERTURE_PROPOSE`。
5. 非 Node0 节点收到 propose 后也 reserve 同一 aperture，并注册到自己的 guest kernel/OBMM。
6. 非 Node0 节点发送 `APERTURE_ACCEPT`；Node0 收齐后发送 `APERTURE_COMMIT`。
7. 任一节点 reserve 或 register 失败，会发送/传播 error，整个 generation 失败。

关键代码：

```c
static void *reserve_aperture(uint64_t base, uint64_t size)
{
    void *addr = (void *)(uintptr_t)base;

    return mmap(addr, size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
}

static int register_kernel_aperture(int obmm_fd,
                                    const struct gva_mgr_config *cfg)
{
    struct obmm_cmd_gsva_aperture req = {0};
    struct obmm_cmd_gsva_aperture query = {0};

    req.base = cfg->aperture_base;
    req.size = cfg->aperture_size;
    req.generation = cfg->generation;
    req.node_id = (uint32_t)cfg->node_id;
    req.node_count = (uint32_t)cfg->node_count;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;

    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0)
        return -1;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0)
        return -1;
    if (!(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE) ||
        query.base != cfg->aperture_base ||
        query.size != cfg->aperture_size ||
        query.generation != cfg->generation)
        return -1;

    log_msg("kernel aperture registry -> ok base=%#" PRIx64
            " size=%#" PRIx64 " generation=%#" PRIx64,
            query.base, query.size, query.generation);
    return 0;
}
```

Node0 的 proposer 逻辑：

```c
if (cfg->node_id == 0) {
    reserved = reserve_aperture(cfg->aperture_base, cfg->aperture_size);
    if (!reserved)
        goto fail;
    log_msg("aperture reserved registry=process-local base=%#" PRIx64
            " size=%#" PRIx64,
            cfg->aperture_base, cfg->aperture_size);
    if (register_kernel_aperture(obmm_fd, cfg) != 0)
        goto fail;

    for (peer = 1; peer < cfg->node_count; peer++)
        send_msg(..., GVA_MGR_MSG_APERTURE_PROPOSE, ...);

    while (pending > 0) {
        recv_msg(..., &msg, &src);
        if (msg.hdr.type == GVA_MGR_MSG_ERROR)
            goto fail;
        if (msg.hdr.type == GVA_MGR_MSG_APERTURE_ACCEPT)
            pending--;
    }

    for (peer = 1; peer < cfg->node_count; peer++)
        send_msg(..., GVA_MGR_MSG_APERTURE_COMMIT, ...);
}
```

Follower 节点逻辑：

```c
cfg->aperture_base = msg.aperture_base;
cfg->aperture_size = msg.aperture_size;
reserved = reserve_aperture(cfg->aperture_base, cfg->aperture_size);
if (!reserved) {
    send_msg(..., GVA_MGR_MSG_ERROR, ..., (uint32_t)saved_errno);
    log_msg("aperture reserve failed base=%#" PRIx64
            " size=%#" PRIx64 " errno=%d",
            cfg->aperture_base, cfg->aperture_size, saved_errno);
    goto fail;
}
if (register_kernel_aperture(obmm_fd, cfg) != 0)
    goto fail;
send_msg(..., GVA_MGR_MSG_APERTURE_ACCEPT, ...);
```

因此，manager bootstrap 日志中的 `bootstrap hello -> ok peers=3`、`aperture reserved registry=process-local`、`kernel aperture registry -> ok`、`result=done registry=kernel-obmm` 分别对应上述四个实现阶段。冲突场景中的 `errno=17` 则来自 `MAP_FIXED_NOREPLACE` reserve 失败，说明保留地址区间确实受进程地址空间占用保护。

### 10.4 `obmm_gsva_demo` 通用实现逻辑

源码：`guest-linux/aarch64/apps/obmm_gsva_demo/obmm_gsva_demo.c`

`obmm_gsva_demo` 的所有正常 GSVA 模式都会先注册 aperture，再进入 identity 或 matrix 数据面。这个 common path 是报告中 `kernel aperture registry -> ok` 的来源。

关键代码：

```c
static int register_aperture(int obmm_fd,
                             const struct gsva_demo_config *cfg,
                             int local_idx)
{
    struct obmm_cmd_gsva_aperture req = {0};
    struct obmm_cmd_gsva_aperture query = {0};
    uint64_t aperture_size = cfg->size;

    if (cfg->mode == GSVA_DEMO_MATRIX)
        aperture_size *= (uint64_t)cfg->node_count;

    req.base = cfg->base;
    req.size = aperture_size;
    req.generation = GSVA_DEMO_GENERATION;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
    req.node_id = (uint32_t)local_idx;
    req.node_count = (uint32_t)cfg->node_count;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0)
        return -1;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0)
        return -1;
    if (query.base != cfg->base || query.size != aperture_size ||
        query.generation != GSVA_DEMO_GENERATION ||
        !(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
```

`main()` 根据 mode 分发：

```c
if (register_aperture(obmm_fd, &cfg, local_idx) != 0) {
    log_msg("aperture register failed errno=%d", errno);
    goto out;
}
log_msg("kernel aperture registry -> ok base=%#" PRIx64 " size=%#"
        PRIx64, cfg.base,
        cfg.mode == GSVA_DEMO_MATRIX ?
        cfg.size * (uint64_t)cfg.node_count : cfg.size);

if (cfg.mode == GSVA_DEMO_MATRIX) {
    ret = run_matrix(obmm_fd, local_cna, &cfg, local_idx,
                     &local_meta) == 0 ? 0 : 1;
    goto out;
}

if (local_idx == 0)
    ret = run_identity_home(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;
else
    ret = run_identity_peer(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;
```

这个 common path 有两个重要含义：

1. identity 模式注册的是一个 `cfg->size` 大小的 GSVA aperture。
2. matrix 模式注册的是 `cfg->size * node_count`，即把每个节点的 slice 放入同一个 global aperture 中。

因此，报告中两节点 identity 的 aperture size 是 `0x400000`，四节点 matrix 的 aperture size 是 `0x1000000`。这两个值都不是日志巧合，而是 demo 按 mode 计算出来的。

### 10.5 GSVA identity demo 逻辑

源码：`guest-linux/aarch64/apps/obmm_gsva_demo/obmm_gsva_demo.c`

实现步骤：

1. 两个节点都先通过 `register_aperture()` 注册同一个 GSVA aperture。
2. Node0 作为 home，调用 `obmm_do_export_fixed_uba(..., cfg->base)`，强制把 export UBA 固定到 `cfg->base`。
3. Node0 把自己的 export metadata 发布到 OBMM bootstrap 区。
4. Node0 在 `cfg->base` 上 mmap export memory，写入 payload magic、初始 value 和 `home_ptr`，然后把 `phase` 置为 1。
5. Node1 作为 peer，通过 OBMM bootstrap lookup 找到 Node0 的 metadata，并检查 `metas[0].remote_uba == cfg->base`。
6. Node1 调用 `obmm_do_import_v2()`，指定 `OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY`，并传入 `local_va=cfg->base`、`home_va=cfg->base`、`pte_offset=0`。
7. Node1 在同一个 `cfg->base` 上 mmap import memory，等待 home payload `phase=1`，校验 magic/value/home_ptr。
8. Node1 写回 `GSVA_DEMO_B` 和 `peer_ptr=cfg->base`，把 `phase` 置为 2。
9. Node0 等到 `phase=2` 后校验 peer 写回值和 `peer_ptr`，成功后输出 `result=done`。

这个流程同时验证了三个层面：fixed UBA export、identity GVA route、双向数据一致性。

Home 侧关键逻辑：

```c
static int run_identity_home(int obmm_fd, uint32_t local_cna,
                             const struct gsva_demo_config *cfg,
                             struct obmm_helpers_meta *local_meta)
{
    local_meta->export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, local_meta, cfg->size,
                                 cfg->base) != 0)
        return -1;
    if (local_meta->remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_unexport;
    }

    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    if (obmm_map_region_at(local_meta->export_mem_id,
                           (void *)(uintptr_t)cfg->base, cfg->size, false,
                           &region) != 0)
        goto out_unexport;

    payload = (struct gsva_demo_payload *)region.addr;
    payload->magic = GSVA_DEMO_MAGIC;
    payload->value = GSVA_DEMO_A;
    payload->home_ptr = cfg->base;
    payload->phase = 1;

    if (wait_phase(&payload->phase, 2) != 0)
        goto out_unmap;
    if (payload->value != GSVA_DEMO_B || payload->peer_ptr != cfg->base) {
        errno = EIO;
        goto out_unmap;
    }

    log_msg("result=done mode=identity role=home ptr=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " value=%#" PRIx64,
            cfg->base, cfg->base, local_meta->remote_uba,
            (uint64_t)payload->value);
    ret = 0;
}
```

Peer 侧关键逻辑：

```c
static int run_identity_peer(int obmm_fd, uint32_t local_cna,
                             const struct gsva_demo_config *cfg,
                             struct obmm_helpers_meta *local_meta)
{
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GSVA_DEMO_GENERATION,
                              metas, got) != 0)
        goto out_unexport;
    if (!got[0] || metas[0].remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_unexport;
    }

    if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0], 0,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                          OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                          0, 0, 0, 0, 0, 1, cfg->base, cfg->base, 0,
                          &import_mem_id) != 0)
        goto out_unexport;

    if (obmm_map_region_at(import_mem_id, (void *)(uintptr_t)cfg->base,
                           cfg->size, import_osync[0], &region) != 0)
        goto out_unimport;

    payload = (struct gsva_demo_payload *)region.addr;
    if (wait_phase(&payload->phase, 1) != 0)
        goto out_unmap;
    if (payload->magic != GSVA_DEMO_MAGIC || payload->value != GSVA_DEMO_A ||
        payload->home_ptr != cfg->base) {
        errno = EIO;
        goto out_unmap;
    }

    payload->value = GSVA_DEMO_B;
    payload->peer_ptr = cfg->base;
    payload->phase = 2;
    log_msg("result=done mode=identity role=peer ptr=%#" PRIx64
            " user_va=%#" PRIx64 " uba=%#" PRIx64 " value=%#" PRIx64,
            cfg->base, cfg->base, metas[0].remote_uba,
            (uint64_t)GSVA_DEMO_B);
    ret = 0;
}
```

这段代码直接对应两节点 GSVA identity 日志：

```text
local_va/home_va/uba/user_va = cfg->base = 0x700000000000
address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY
pte_offset = 0
```

因此，日志中的 `user_va=0x700000000000 uba=0x700000000000` 与 QEMU 侧 `address_profile=2 pte_offset=0` 是 demo 逻辑主动设置并验证的结果。

### 10.6 GSVA matrix demo 逻辑

源码：`guest-linux/aarch64/apps/obmm_gsva_demo/obmm_gsva_demo.c`

实现步骤：

1. 每个节点根据 `local_idx` 计算自己的 `local_base = matrix_slot_base(cfg, local_idx)`。
2. 每个节点调用 `obmm_do_export_fixed_uba(..., local_base)`，把自己的 slice 固定 export 到全局地址区间中的专属 slot。
3. 每个节点把自己的 metadata 发布到 OBMM bootstrap 区，并在 `local_base` mmap 自己的 export memory。
4. 每个节点通过 `obmm_bootstrap_lookup()` 获取所有节点 metadata。
5. 对于每个远端 owner，当前节点检查 `metas[owner].remote_uba == slot_base`，防止导入地址与全局布局不一致。
6. 对每个远端 owner 调 `obmm_do_import_v2()`，指定 `GSVA_IDENTITY` profile，传入 `local_va=slot_base`、`home_va=slot_base`、`pte_offset=0`。
7. 当前节点在每个远端 `slot_base` 上 mmap import memory，因此本节点地址空间里能看到所有节点的 GSVA slice。
8. 每个节点向所有 owner 的 payload 中写入 `matrix_value(local_idx, owner)`。
9. 每个节点等待所有 owner 上所有 writer 的 value 都达到预期，全部满足后输出 `result=done mode=matrix`。

这个流程验证的不是单向共享，而是 full-mesh：每个节点都必须能访问其他 `node_count - 1` 个 GSVA slice，并且每个 writer 的写入都能被对应 owner 读到。

关键逻辑：

```c
static int run_matrix(int obmm_fd, uint32_t local_cna,
                      const struct gsva_demo_config *cfg, int local_idx,
                      struct obmm_helpers_meta *local_meta)
{
    uint64_t local_base = matrix_slot_base(cfg, local_idx);
    int import_count = cfg->node_count - 1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, local_meta, cfg->size,
                                 local_base) != 0)
        return -1;
    if (local_meta->remote_uba != local_base) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (obmm_bootstrap_publish(obmm_fd, local_idx, cfg->node_count,
                               GSVA_DEMO_GENERATION, local_meta) != 0)
        goto out_unexport;
    if (obmm_map_region_at(local_meta->export_mem_id,
                           (void *)(uintptr_t)local_base, cfg->size, false,
                           &regions[local_idx]) != 0)
        goto out_unexport;

    if (obmm_bootstrap_lookup(obmm_fd, local_cna, cfg->node_count,
                              GSVA_DEMO_GENERATION, metas, got) != 0)
        goto out_cleanup;

    if (!obmm_alloc_import_pas(import_count, cfg->size, import_pas,
                               import_osync, obmm_parse_import_cache_mode()))
        goto out_cleanup;

    for (owner = 0; owner < cfg->node_count; owner++) {
        uint64_t slot_base = matrix_slot_base(cfg, owner);

        if (owner == local_idx)
            continue;
        if (!got[owner] || metas[owner].remote_uba != slot_base ||
            metas[owner].size != cfg->size) {
            errno = EINVAL;
            goto out_cleanup;
        }
        if (obmm_do_import_v2(obmm_fd, &metas[owner], local_cna,
                              import_pas[import_idx], 0,
                              OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                              OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                              0, 0, 0, 0, 0, (uint64_t)owner + 1,
                              slot_base, slot_base, 0,
                              &import_mem_id[owner]) != 0)
            goto out_cleanup;
        if (obmm_map_region_at(import_mem_id[owner],
                               (void *)(uintptr_t)slot_base, cfg->size,
                               import_osync[import_idx],
                               &regions[owner]) != 0)
            goto out_cleanup;
        import_idx++;
    }
}
```

数据一致性检查逻辑：

```c
for (owner = 0; owner < cfg->node_count; owner++)
    payloads[owner]->values[local_idx] = matrix_value(local_idx, owner);

for (owner = 0; owner < cfg->node_count; owner++) {
    for (writer = 0; writer < cfg->node_count; writer++) {
        uint64_t expect = matrix_value(writer, owner);

        while (obmm_now_ms() < deadline &&
               payloads[owner]->values[writer] != expect) {
            usleep(1000);
        }
        if (payloads[owner]->values[writer] != expect) {
            errno = ETIMEDOUT;
            goto out_cleanup;
        }
    }
}

log_msg("result=done mode=matrix node=%d node_count=%d slice_base=%#"
        PRIx64 " ptr=%#" PRIx64 " value_from_node0=%#" PRIx64
        " value_from_last=%#" PRIx64,
        local_idx, cfg->node_count, local_base, local_base,
        (uint64_t)payloads[local_idx]->values[0],
        (uint64_t)payloads[local_idx]->values[cfg->node_count - 1]);
```

这段代码直接解释了四节点 matrix 日志为什么需要每节点三条 `GVA_S3_MAP`：每个节点跳过本地 owner，然后对其他 `node_count - 1` 个 owner 执行一次 `obmm_do_import_v2()`。同时，`slot_base, slot_base, 0` 明确要求：

```text
local_va == home_va == uba == slot_base
pte_offset == 0
address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY
```

因此，四节点日志中的三条 route 和 `value_from_node0/value_from_last` 不是偶然输出，而是 demo 对 full-mesh GSVA slice 的显式校验结果。

### 10.7 GVA direct demo 逻辑

源码：`guest-linux/aarch64/apps/gva_direct_demo/gva_direct_demo.c`

默认地址配置：

```c
#define GVA_DIRECT_LOCAL_VA 0x710000000000ULL
#define GVA_DIRECT_HOME_VA  0x720000000000ULL
```

实现步骤：

1. Node0 作为 home，用普通 `obmm_do_export()` 导出 memory。这个 export 不要求 UBA 等于用户 VA。
2. Node0 通过 OBMM bootstrap 发布 export metadata，并把 export memory mmap 到 `cfg->home_va`。
3. Node0 写入 payload magic、初始 value 和 `home_ptr=cfg->home_va`，把 `phase` 置为 1。
4. Node1 作为 peer，通过 OBMM bootstrap lookup 获取 Node0 metadata。
5. Node1 计算 `pte_offset = metas[0].remote_uba - cfg->local_va`。这是 generic GVA 路径的核心：peer 用户地址需要通过 offset 转成 remote UBA。
6. Node1 调用 `obmm_do_import_v2()`，指定 `OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA`，传入 `local_va=cfg->local_va`、`home_va=cfg->home_va`、`pte_offset=pte_offset`。
7. Node1 在 `cfg->local_va` 上 mmap import memory，读取 home payload 并校验 `home_ptr == cfg->home_va`。
8. Node1 写回 `GVA_DIRECT_B` 和 `peer_ptr=cfg->local_va`，把 `phase` 置为 2。
9. Node0 等到 `phase=2` 后校验 peer 写回值和 `peer_ptr`，成功后输出 `result=done`。

这个流程刻意让 `local_va`、`home_va`、`uba` 三者不相等，用来证明普通 GVA 的 offset 转换路径，而不是 GSVA identity 路径。

Home 侧关键逻辑：

```c
static int run_home(int obmm_fd, uint32_t local_cna,
                    const struct gva_direct_config *cfg,
                    struct obmm_helpers_meta *local_meta)
{
    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GVA_DIRECT_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    if (obmm_map_region_at(local_meta->export_mem_id,
                           (void *)(uintptr_t)cfg->home_va,
                           cfg->size, false, &region) != 0)
        goto out_unexport;

    payload = (struct gva_direct_payload *)region.addr;
    payload->magic = GVA_DIRECT_MAGIC;
    payload->value = GVA_DIRECT_A;
    payload->home_ptr = cfg->home_va;
    payload->phase = 1;

    if (wait_phase(&payload->phase, 2) != 0)
        goto out_unmap;
    if (payload->value != GVA_DIRECT_B || payload->peer_ptr != cfg->local_va) {
        errno = EIO;
        goto out_unmap;
    }

    log_msg("result=done mode=%s role=home local_va=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " pte_offset=remote-local"
            " value=%#" PRIx64 " sync_done=%" PRIu64,
            mode_name(cfg->mode), cfg->local_va, cfg->home_va,
            local_meta->remote_uba, (uint64_t)payload->value,
            (uint64_t)payload->sync_done);
    ret = 0;
}
```

Peer 侧关键逻辑：

```c
static int run_peer(int obmm_fd, uint32_t local_cna,
                    const struct gva_direct_config *cfg,
                    struct obmm_helpers_meta *local_meta)
{
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GVA_DIRECT_GENERATION,
                              metas, got) != 0)
        goto out_unexport;

    pte_offset = metas[0].remote_uba - cfg->local_va;

    if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0],
                          token_value,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                          cache_policy,
                          0, 0, 0, p_tag, access_flags, 1, cfg->local_va,
                          cfg->home_va, pte_offset, &import_mem_id) != 0)
        goto out_unexport;

    log_msg("guest_route_dump map_source=%u address_profile=%u cache_policy=%u "
            "gva_id=%u local_va=%#" PRIx64 " home_va=%#" PRIx64
            " pte_offset=%#" PRIx64 " uba=%#" PRIx64 " import_pa=%#"
            PRIx64,
            OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
            OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
            OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH, 1, cfg->local_va,
            cfg->home_va, pte_offset, metas[0].remote_uba, import_pas[0]);

    if (obmm_map_region_at(import_mem_id, (void *)(uintptr_t)cfg->local_va,
                           cfg->size, import_osync[0], &region) != 0)
        goto out_unimport;

    payload = (struct gva_direct_payload *)region.addr;
    if (payload->magic != GVA_DIRECT_MAGIC || payload->value != GVA_DIRECT_A ||
        payload->home_ptr != cfg->home_va) {
        errno = EIO;
        goto out_unmap;
    }

    payload->value = GVA_DIRECT_B;
    payload->peer_ptr = cfg->local_va;
    payload->phase = 2;

    log_msg("result=done mode=%s role=peer local_va=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " pte_offset=%#"
            PRIx64 " value=%#" PRIx64,
            mode_name(cfg->mode), cfg->local_va, cfg->home_va,
            metas[0].remote_uba, pte_offset, (uint64_t)GVA_DIRECT_B);
    ret = 0;
}
```

这段代码直接对应两节点 GVA direct 日志：

```text
local_va = 0x710000000000
home_va  = 0x720000000000
uba      = metas[0].remote_uba
pte_offset = uba - local_va
address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA
```

因此，`address_profile=1` 和非零 `pte_offset=0x8effffc00000` 是 GVA direct demo 的核心验证点。peer 侧在 `local_va` 上 mmap import memory，但 QEMU/SIM_DEC route 将访问转换到 `remote_uba`，这正是普通 GVA 与 GSVA identity 的关键区别。

### 10.8 代码和日志之间的对应关系

| 代码行为 | 日志证据 | 证明点 |
| --- | --- | --- |
| `obmm_do_export_fixed_uba(..., cfg->base)` | `fixed export -> ok ... uba=0x700000000000` | GSVA home 侧在指定 global VA 上 export |
| `obmm_do_import_v2(..., GSVA_IDENTITY, ..., cfg->base, cfg->base, 0)` | `GVA_S3_MAP ... local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 address_profile=2` | GSVA identity route 生效 |
| matrix 对每个 remote owner 执行 `obmm_do_import_v2()` | 每节点三条 `GVA_S3_MAP ... address_profile=2` | 四节点 full-mesh GSVA slice 全互联 |
| matrix 写入并等待 `payloads[owner]->values[writer]` | `value_from_node0=... value_from_last=...` | 跨节点数据一致 |
| GVA direct 计算 `pte_offset = metas[0].remote_uba - cfg->local_va` | `pte_offset=0x8effffc00000 address_profile=1` | 普通 GVA offset 映射生效 |
| peer 在 `cfg->local_va` mmap import memory | `GVA_PATH ... local_va=710000000000 ... remote_uba=ffffffc00008` | CPU window 访问被 QEMU 路由到 remote UBA |

这些代码片段使前面的运行日志有了明确实现来源：当前状态不是单纯“日志里有 PASS”，而是 demo 在 guest 侧主动构造 GSVA/GVA 映射、触发 QEMU/SIM_DEC route，并用 payload 双向读写验证访问结果。

## 11. 复现命令

以下命令对应本报告引用的脚本入口。实际运行会生成新的 `RUN_ID`，报告中的证据来自第 3 节列出的已完成日志。

```bash
./guest-linux/aarch64/scripts/run_ub_dual_node_gsva_manager_bootstrap.sh
./guest-linux/aarch64/scripts/run_ub_dual_node_gsva_demo.sh
./guest-linux/aarch64/scripts/run_ub_dual_node_gva_direct_test.sh
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_manager_bootstrap.sh
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_matrix_demo.sh
EXPECT_FAILURE=1 GVA_MANAGER_CONFLICT_NODE=2 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_manager_bootstrap.sh
```

## 12. 剩余风险与边界

1. 当前报告证明的是 `ub_sim` 当前分阶段架构下的 GVA/GSVA 可用性；QEMU 中仍由 SIM_DEC 承载部分 S3/GVA 路由模拟，并不等同于最终硬件级 S3/NOC 完整模型。
2. 当前两节点 GSVA identity 与四节点 matrix 已覆盖 `pte_offset=0` 的 GSVA 主路径；更复杂的 mmap flag/API 管理面、权限隔离、多进程生命周期、回收语义仍需要后续专项测试。
3. 当前 GVA direct 已覆盖 `pte_offset != 0` 的普通 GVA 路径；更多异常路径，例如 invalid token、invalid UPI、invalid p_tag、unmap fault，可以基于已有 direct test mode 继续扩展报告。
4. 当前报告基于已完成日志做事实归档，没有重新触发新一轮 QEMU 运行。

## 13. 最终判定

本次日志能够证明：

| 场景 | 判定 |
| --- | --- |
| 2-node GSVA manager bootstrap | PASS |
| 2-node GSVA identity | PASS |
| 2-node GVA direct write-read | PASS |
| 4-node GSVA manager bootstrap | PASS |
| 4-node GSVA matrix | PASS |
| 4-node GSVA aperture conflict | PASS，按预期失败 |

最终结论：当前 `ub_sim` 已经能够在两节点与四节点 QEMU 仿真环境中正确模拟 GVA 与 GSVA 的核心行为，包括 GSVA global shared virtual address 的 identity 映射、普通 GVA 的 offset 映射、S3 route 注册、跨节点 CPU window 访问、读写统计和 aperture 冲突检测。
