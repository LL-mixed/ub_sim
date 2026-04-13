# 双节点 UB 互联环境 URMA 数据面功能验证报告

| 项目 | 值 |
|------|-----|
| 日期 | 2026-04-06 |
| 测试环境 | QEMU 8.2.0 + UB 定制 (aarch64) |
| 拓扑 | 双节点直连 nodeA.ubcdev0:1 <-> nodeB.ubcdev0:1 |
| 帧协议 | Spec 对齐 MsgPktHeader (32B 固定头 + plen payload) |
| 测试脚本 | `run_ub_dual_node_ubcore_urma_e2e.sh` |
| QEMU 参数 | `-M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on -cpu cortex-a57 -m 8G` |

---

## 1. 测试概览

| 测试项 | 状态 | 说明 |
|--------|------|------|
| ub_link Socket 连接 | PASS | nodeA server, nodeB client (第 6 次尝试连接成功) |
| 远端快照同步 | PASS | 双节点快照加载、cfg notify 均正常 |
| Business Message | PASS (3 iter) | bizmsg 往返验证，payload 一致性 3/3 通过 |
| URMA JFS/JFR/JFC 创建 | PASS (nodeA) | nodeA 成功创建 4 JFS, 4 JFR, 8 JFC |
| URMA SEND (nodeA -> nodeB) | PASS | 90 字节数据通过 ub_link 发送，nodeB 收到 |
| URMA RX (nodeB 接收) | PARTIAL | nodeB 收到 URMA 数据包但 jetty 未激活 |
| URMA Dataplane E2E | FAIL | nodeA 超时等待 nodeB 回包 |

---

## 2. 控制面验证（已通过）

### 2.1 ub_link Socket 连接

| 节点 | 角色 | 事件 |
|------|------|------|
| nodeA | server | `ub_link: server listening on /tmp/ub-qemu-links-dual/nodeA_ubcdev0__1.sock` |
| nodeB | client | `ub_link: client connected (attempt 6)` |
| nodeA | accept | `ub_link: accepted incoming ulink connection` |

Socket 全生命周期正常：listen -> connect -> accept -> AIO watch。

### 2.2 远端快照同步

| 节点 | 对端 | EID | UPI | fm_cna |
|------|------|-----|-----|--------|
| nodeA | nodeB | 400734 | 32767 (0x7fff) | 0xc4d2 |
| nodeB | nodeA | 400733 | 32767 (0x7fff) | 0xc4c2 |

流程完整：remote cfg path -> snapshot load -> cfg notify。

### 2.3 Business Message 往返

| 指标 | nodeA | nodeB |
|------|-------|-------|
| IRQ delta (hi_msgq0-0) | 52 (before=99, after=151) | 47 (before=94, after=141) |
| Payload 一致性 | 3/3 pass | 3/3 pass |
| Roundtrip | pass | pass |

Payload 检查覆盖 upi_case0/1/2 (mask=0x7fff)，tx/rx 数据完全一致：

| case | offset | tx | rx | match |
|------|--------|----|----|-------|
| upi_case0 | 0x7c | 0x1335 | 0x1335 | YES |
| upi_case1 | 0x7c | 0x2a5a | 0x2a5a | YES |
| upi_case2 | 0x7c | 0x55a5 | 0x55a5 | YES |

### 2.4 驱动加载

| 驱动 | nodeA | nodeB |
|------|-------|-------|
| hisi_ubus.ko | exit=0 | exit=0 |
| udma.ko | exit=0 | exit=0 |
| linqu_ub_drv.ko | exit=0 | exit=0 |
| ubcore client | Register success | Register success |
| ipourma netlink | Register success | Register success |

---

## 3. URMA 数据面验证（部分通过 / 最终失败）

### 3.1 URMA 资源创建（nodeA）

nodeA 成功创建了完整的 URMA 发送/接收资源：

**JFC (Completion Queue):**

| jfc_id | cq_buf | cq_depth |
|--------|--------|----------|
| 64 | 0x100042ffff8000 | 4 |
| 65 | 0x100042ffff8000 | 4 |
| 66 | 0x100042ffff8000 | 4 |
| 67 | 0x100042ffff8000 | 4 |
| 68 | 0x100042ffff8000 | 4 |
| 69 | 0x100042ffff8000 | 4 |
| 70 | 0x100042ffff8000 | 4 |
| 71 | 0x100042ffff8000 | 4 |

**JFR (Receive Queue):**

| jfr_id | rq_buf | rq_depth |
|--------|--------|----------|
| 64 | 0xffff8000804a1000 | 1024 |
| 65 | 0xffff8000820f1000 | 1024 |
| 66 | 0xffff8000800ad000 | 256 |
| 67 | 0xffff800082121000 | 1024 |

**JFS (Send Queue):**

| jetty_id | sq_buf | sq_depth | tx_jfcn | jfs_mode | seid_idx |
|----------|--------|----------|---------|----------|----------|
| 5 | 0xffff8000820bf000 | 2048 | 66 | 1 | 0 |
| 1 | 0xffff8000820f6000 | 1024 | 68 | 1 | 0 |
| 32 | 0xffff800082146000 | 512 | 64 | 1 | 0 |
| 2 | 0xffff80008214f000 | 1024 | 70 | 1 | 0 |

### 3.2 URMA SEND 数据发送（nodeA）

nodeA 通过 jetty_id=32 成功发送了 URMA 数据包：

```
ubc doorbell: jetty_id=32 addr=0x21080 val=1 active=1
ubc doorbell: jetty_id=32 new_pi=1 sq_ci=0
ubc WQE raw: dw0=0xa0200000 dw1=0 dw2=0x1000000 dw3=0x20 dw4=0x2 dw5=0 dw6=0 dw7=0xff020000
ubc WQE: jetty=32 idx=0 op=0x00 inline=0 sge_num=1 inline_len=0 rmt_obj_id=32
ubc WQE SGE[0]: va=0xffff41a7427c0000 len=58
ubc SEND: jetty=32 rmt_obj_id=32 payload_len=58
ubc SEND: sent 90 bytes (hdr=32 data=58) via ub_link
ubc CQE: jetty=32 wqe_idx=0 byte_cnt=0 cq_pi=1 jfc=64
```

发送路径完整：doorbell -> WQE 解析 -> SGE 地址翻译 -> SEND 封包 -> ub_link 发送 -> CQE 完成。

### 3.3 URMA RX 数据接收（nodeB）

nodeB 确实收到了 nodeA 发送的 URMA 数据包：

```
ub_link: packet rx cfg=6 plen=58 total=90
ubc_msgq: received remote msg code=7 len=90
ubc URMA RX: dst_jetty=32 len=58
ubc URMA RX: dst jetty 32 not active
```

**关键发现**：数据包已通过 ub_link 到达 nodeB，但 nodeB 的 jetty 32 尚未激活。

### 3.4 URMA Dataplane 测试结果

| 参数 | 值 |
|------|-----|
| 角色 | nodeA=10.0.0.1, nodeB=10.0.0.2 |
| 接口 | ipourma0 (ifindex=3) |
| 测试方式 | UDP socket, broadcast + unicast |
| 超时 | 30s |

**nodeA 日志：**
```
[urma_dp] start role=nodeA
[urma_dp] iface=ipourma0 ifindex=3 local=10.0.0.1 peer=10.0.0.2
[urma_dp] warn: SO_BINDTODEVICE failed: Protocol not available
...
[urma_dp] fail: timeout waiting peer packet
[init] urma dataplane fail exit=1
```

**nodeB：** urma_dp 进程未启动（QEMU 在 nodeA 失败后被终止）。

---

## 4. 根因分析

与上一轮测试（2026-04-05）相比，本轮取得了显著进展：数据包已能通过 ub_link 从 nodeA 到达 nodeB。但仍未能完成 E2E 收发，根因如下。

### 4.1 nodeB URMA 资源未创建

nodeB 在收到 nodeA 发送的 URMA 数据包时尚未完成自身的 URMA 初始化：
- nodeB 没有任何 CREATE_JFC / CREATE_JFR / CREATE_JFS 日志
- nodeB 的 jetty 32 未激活，无法将收到的数据包投递到用户态

**原因**：nodeB 在 bizmsg roundtrip 完成后进入了 ubase 初始化阶段（entity enable），此时大量 POST_MB 命令因 QEMU 未实现对应的 sub_op 处理而被丢弃，导致 ubase 初始化缓慢或卡住，后续的 URMA 资源创建无法进行。

### 4.2 POST_MB 未实现的 sub_op

| sub_op | nodeA 出现次数 | nodeB 出现次数 | 推测功能 |
|--------|---------------|---------------|----------|
| 0x14 | 64 | 0 | 页表/内存区域配置 |
| 0x44 | 16 | 9 | 页表/TLB 操作 |
| 0x34 | 1 | 1 | 初始化/查询 |
| 0x00 | 1 | 0 | 通用配置 |
| 0x10 | 1 | 0 | 资源管理 |
| 0x20 | 1 | 0 | 状态查询 |
| 0x50 | 1 | 0 | 资源分配 |
| 0x55 | 1 | 0 | 资源操作 |
| 0x60 | 1 | 0 | DMA 配置 |

未实现的 sub_op 合计 87 条（nodeA）和 10 条（nodeB），这些命令的丢弃导致 ubase/urma 初始化链路不完整。

### 4.3 UMMU 地址翻译失败

nodeB 日志中反复出现 UMMU 翻译错误：

```
ummu_translate: addr(0xfffffff8000), translated_addr(0x102027000)
...
invalid or reserved pte.
ummu_translate: addr(0x8090040), translated_addr(0x8090040)
report event EVT_A_TRANSLATION: tecte_tag 0 tid 65
```

DMA 操作触发的地址翻译持续失败（invalid pte），产生 EVT_A_TRANSLATION 事件。这导致 ubase 在初始化过程中反复超时：

```
ubase 00001: entity enable, ret=-110, enable=1   # -110 = ETIMEDOUT
ub_bus_controller0: task 0 msn 0x57 wait cqe timeout
```

### 4.4 SO_BINDTODEVICE 失败

`SO_BINDTODEVICE failed: Protocol not available` 是次要问题，可能由于 QEMU virtio 网络栈不支持此 socket 选项。但 ipourma0 接口实际存在且可用（数据包已通过 URMA SEND 机制发送成功）。

---

## 5. 与上一轮测试的进展对比

| 指标 | 上一轮 (04-05) | 本轮 (04-06) | 变化 |
|------|---------------|-------------|------|
| ub_link Socket | PASS | PASS | - |
| 快照同步 | PASS | PASS | - |
| bizmsg roundtrip | PASS | PASS | - |
| JFS/JFR 创建 | FAIL (jetty 创建失败) | PASS (nodeA 4 JFS + 4 JFR) | **改进** |
| SEND 发送 | 未到达对端 | 通过 ub_link 发送成功 | **改进** |
| 对端接收 | 未收到 | 收到 URMA RX 但 jetty 未激活 | **改进** |
| E2E 结果 | FAIL | FAIL | 仍失败 |

**关键进展**：本轮测试中 nodeA 的 URMA 发送通道已完全打通（JFS 创建 -> doorbell -> WQE -> SEND -> CQE），数据包通过 ub_link 成功到达 nodeB。问题从"数据无法发送"演进为"接收端资源未就绪"。

---

## 6. 结论

| 层级 | 能力 | 状态 |
|------|------|------|
| L1: Socket 传输层 | ub_link Unix socket 连接 | PASS |
| L2: 控制消息层 | cfg/snapshot/notify 消息传递 | PASS |
| L3: UB 消息队列层 | hi_msgq 中断、bizmsg 往返、payload 一致性 | PASS |
| L4: URMA 发送资源 | JFC/JFR/JFS 创建、WQE/SGE/doorbell 机制 | PASS (nodeA) |
| L5: URMA 数据发送 | SEND 封包、ub_link 传输 | PASS |
| L6: URMA 数据接收 | 对端 RX 收包 | PARTIAL (收到包但 jetty 未激活) |
| L7: URMA E2E 收发 | 双向 UDP over ipourma0 | FAIL |

控制面（L1-L3）已完全通过。数据面发送路径（L4-L5）也已打通。数据面接收路径（L6-L7）的阻塞点在于 nodeB 的 URMA 接收资源未能及时创建。

---

## 7. 后续工作建议

| 优先级 | 工作项 | 说明 |
|--------|--------|------|
| P0 | 实现 POST_MB sub_op=0x14 | 出现 64 次，是页表/内存区域配置，阻塞 ubase 初始化 |
| P0 | 实现 POST_MB sub_op=0x44 | 出现 16+9 次，TLB/页表操作，影响 DMA 地址翻译 |
| P1 | 修复 UMMU invalid pte | 排查 `invalid or reserved pte` 原因，确保 DMA 翻译链路完整 |
| P1 | 调整测试启动时序 | 确保 nodeB 的 URMA 资源创建完成后再开始数据面测试 |
| P2 | 实现 POST_MB sub_op=0x34/0x00/0x10/0x20/0x50/0x55/0x60 | 补全所有未实现的邮箱命令 |
| P2 | 修复 SO_BINDTODEVICE | 排查 ipourma0 的 socket 绑定失败 |
