# 四节点 OBMM 无锁队列验证报告

## 范围

本文档验证 OBMM 共享内存池无锁队列 demo（`obmm_queue_demo`）在四节点全互联 QEMU
拓扑上的功能正确性。验证覆盖完整数据路径：OBMM 导出/引入、SPSC 队列描述符交换、
生产者拥有型载荷传输、以及最大队列深度下的压力填满/排空测试。

这是 OBMM 共享内存池之上无锁队列层的首次验证，同时验证了 FM（Fabric Manager）
bootstrap 元数据交换通道。

## 组件版本

- `simulator`
  - `63ba688` `Use FM bootstrap for OBMM queue demo`
  - `a76f567` `Ignore local queue demo build artifacts`
  - `9f18001` `Add OBMM lockless queue demo`
  - `87443a9` `Document OBMM lockless queue design`
- `simulator/guest-linux/kernel_ub`
  - `1fc23c57ce50` `Add OBMM FM bootstrap ioctls`
  - `dc71d27fdbb1` `Fix OBMM remote range sync semantics`
- `simulator/vendor/qemu_8.2.0_ub`
  - `fe0526e257` `Add OBMM bootstrap registry to SIM_DEC`
  - `c09c77b852` `Fix SIM decoder unmap lifetime`
  - `320c88327b` `Flush UAPI completions on CQ reads`

## 拓扑

- [ub_topology_four_node_full_mesh.ini](../vendor/ub_topology_four_node_full_mesh.ini)
- 4 节点：`nodeA` (10.0.0.1)、`nodeB` (10.0.0.2)、`nodeC` (10.0.0.3)、`nodeD` (10.0.0.4)
- 6 条直连链路，每节点 3 个活跃端口
- QEMU：`virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on`，`cortex-a57`，8G 内存，4 vCPU

## 配置

| 参数 | 值 |
|------|----|
| 导出大小 | 每节点 512 MB |
| 队列深度 | 1024 个描述符 |
| Bootstrap 模式 | FM（OBMM_BOOTSTRAP=fm） |
| 内核启动参数 | `linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0 obmm.skip_cache_maintain=1` |

## 验证运行

- 运行目录：
  - [2026-05-06_10-14-08_obmmqueue4_21340_headless4](../guest-linux/aarch64/logs/2026-05-06_10-14-08_obmmqueue4_21340_headless4)
- 汇总：
  - [four_node_obmm_queue_demo.latest.txt](../guest-linux/aarch64/out/four_node_obmm_queue_demo.latest.txt)
- 结果：**PASS**

## 各节点结果

### nodeA（10.0.0.1，node=1）

| 阶段 | 状态 | 详情 |
|------|------|------|
| Export | ok | mem_id=1, 512MB |
| Layout | ok | dir_entries=4, queues=3, queue_depth=1024, tx_arena=524191KB |
| Bootstrap (FM) | ok | count=4 |
| Import | ok | peer=2,3,4 |
| Pool ready | ok | nodes=4 |
| Round 1（owner） | ok | ACK from node=2,3,4; commit ok |
| Round 2（非 owner） | ok | DATA 验证通过 owner=2；发送 ACK；收到 COMMIT |
| Round 3（非 owner） | ok | DATA 验证通过 owner=3；发送 ACK；收到 COMMIT |
| Round 4（非 owner） | ok | DATA 验证通过 owner=4；发送 ACK；收到 COMMIT |
| Stress（owner） | ok | pass=1 fill/drain depth=1024; pass=2 fill/drain depth=1024 |
| Stress（drain） | ok | drained owner=2,3,4 passes=2 depth=1024 |
| 总体 | **pass** | |

### nodeB（10.0.0.2，node=2）

| 阶段 | 状态 | 详情 |
|------|------|------|
| Export | ok | mem_id=1, 512MB |
| Layout | ok | dir_entries=4, queues=3, queue_depth=1024, tx_arena=524191KB |
| Bootstrap (FM) | ok | count=4 |
| Import | ok | peer=1,3,4 |
| Pool ready | ok | nodes=4 |
| Round 1（非 owner） | ok | DATA 验证通过 owner=1；发送 ACK；收到 COMMIT |
| Round 2（owner） | ok | ACK from node=1,3,4; commit ok |
| Round 3（非 owner） | ok | DATA 验证通过 owner=3；发送 ACK；收到 COMMIT |
| Round 4（非 owner） | ok | DATA 验证通过 owner=4；发送 ACK；收到 COMMIT |
| Stress（drain） | ok | drained owner=1,3,4 passes=2 depth=1024 |
| Stress（owner） | ok | pass=1,2 fill/drain depth=1024 |
| 总体 | **pass** | |

### nodeC（10.0.0.3，node=3）

| 阶段 | 状态 | 详情 |
|------|------|------|
| Export | ok | mem_id=1, 512MB |
| Layout | ok | dir_entries=4, queues=3, queue_depth=1024, tx_arena=524191KB |
| Bootstrap (FM) | ok | count=4 |
| Import | ok | peer=1,2,4 |
| Pool ready | ok | nodes=4 |
| Round 1（非 owner） | ok | DATA 验证通过 owner=1；发送 ACK；收到 COMMIT |
| Round 2（非 owner） | ok | DATA 验证通过 owner=2；发送 ACK；收到 COMMIT |
| Round 3（owner） | ok | ACK from node=1,2,4; commit ok |
| Round 4（非 owner） | ok | DATA 验证通过 owner=4；发送 ACK；收到 COMMIT |
| Stress（drain） | ok | drained owner=1,2,4 passes=2 depth=1024 |
| Stress（owner） | ok | pass=1,2 fill/drain depth=1024 |
| 总体 | **pass** | |

### nodeD（10.0.0.4，node=4）

| 阶段 | 状态 | 详情 |
|------|------|------|
| Export | ok | mem_id=1, 512MB |
| Layout | ok | dir_entries=4, queues=3, queue_depth=1024, tx_arena=524191KB |
| Bootstrap (FM) | ok | count=4 |
| Import | ok | peer=1,2,3 |
| Pool ready | ok | nodes=4 |
| Round 1（非 owner） | ok | DATA 验证通过 owner=1；发送 ACK；收到 COMMIT |
| Round 2（非 owner） | ok | DATA 验证通过 owner=2；发送 ACK；收到 COMMIT |
| Round 3（非 owner） | ok | DATA 验证通过 owner=3；发送 ACK；收到 COMMIT |
| Round 4（owner） | ok | ACK from node=1,2,3; commit ok |
| Stress（drain） | ok | drained owner=1,2,3 passes=2 depth=1024 |
| Stress（owner） | ok | pass=1,2 fill/drain depth=1024 |
| 总体 | **pass** | |

## 内核健康

- 四个 guest 日志中均无 `Call trace`、`panic`、`BUG:`、`WARNING:`、`Oops`。
- OBMM import/unmap 生命周期完整：四个节点各成功引入 3 个对端 region，完成后正确 unmap。

## FM Bootstrap 通道

本次验证使用 FM bootstrap 模式，由 OBMM 内核驱动的 `OBMM_CMD_BOOTSTRAP_PUBLISH` 和
`OBMM_CMD_BOOTSTRAP_LOOKUP` ioctl 实现元数据交换。各节点将导出信息（mem_id、UBA、
token、CNA）通过 FM 发布，其他节点通过 lookup 获取。

涉及的完整软件栈：

- **Guest 内核**：`obmm_core.c` 实现 bootstrap ioctl，将记录转发给 SIM Decoder。
- **QEMU SIM Decoder**：`fe0526e257` 新增 bootstrap registry，在设备内部维护
  publish/lookup 存储，不依赖网络。
- **用户态 demo**：`obmm_pool_helpers.h` 中 `obmm_bootstrap_publish()` 和
  `obmm_bootstrap_lookup()` 调用对应 ioctl。

与 UDP bootstrap 相比，FM 模式的优势：

- 不需要 UDP 端口和网络配置，完全在 UB 设备内部完成。
- 元数据传递延迟更低（无需等待 UDP 报文往返）。
- 与真实硬件上的 FM 服务路径一致。

## 队列设计属性验证

### SPSC 正确性

12 条 per-pair 队列（4 节点 x 3 对端）在 rounds 阶段双向验证：

- **生产者 push**：owner 向所有对端 ingress queue 推送 DATA 描述符。
- **消费者 pop**：对端弹出描述符，通过 osync 远程读验证载荷。
- **ACK 路径**：对端向 owner 的 ingress queue 推送 ACK。
- **COMMIT 广播**：owner 收到全部 ACK 后推送 COMMIT。

### 队列深度边界

Stress 阶段将每个对端队列填满至最大深度（1024 个描述符）然后排空。验证了：

- `obmm_spsc_push` 在队列满时正确返回 `-EAGAIN`。
- `obmm_spsc_pop` 在队列空时正确返回 `-EAGAIN`。
- 环形缓冲区越过 `depth` 后的回绕正确性。

### 缓存一致性契约

队列设计将描述符放在目的节点的导出内存中：

- **生产者（远端）**：通过 osync（非缓存）import 映射写入描述符。
- **消费者（本地）**：从本地 cacheable export 映射读取描述符。

这一不对称性是设计的核心，在 stress 阶段以满队列深度进行了充分验证。
未观察到数据陈旧或一致性问题。

### 生产者拥有型载荷

载荷数据写入生产者的本地 cacheable TX arena。仅将 32 字节描述符
（region_id + payload_offset）推送到远端队列。消费者通过其 import 的 osync
映射读取载荷。4 轮 ownership 和全部 stress pass 均验证通过。

## 结论

1. OBMM 无锁 SPSC 队列层在四节点全互联拓扑上功能正确。
2. 12 条 per-pair 队列（4 节点 x 3 ingress queue）全部通过轮转 ownership
   和压力填满/排空测试。
3. 生产者拥有型载荷模型正确工作：本地 cacheable 写入通过远端 osync 读可见。
4. FM bootstrap 通道（内核 ioctl + QEMU SIM Decoder registry）工作正常，
   可作为 UDP bootstrap 的替代方案。
5. 无内核警告、panic 或数据完整性故障。
