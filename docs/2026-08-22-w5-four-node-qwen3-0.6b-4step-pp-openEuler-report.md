# W5 四节点 Qwen3-0.6B 4-step PP —— openEuler guest 验证报告

- 日期：2026-08-22
- 代码基线：`cbe21a8`（openEuler 引擎）；kernel_ub `70c7272`；qemu 子模块 `3a247cb`
- 结论：**PASS**——4 节点 openEuler guest 上 W5 qwen3-0.6B 4-step 流水线并行推理
  端到端退出码 0，输出与 busybox 基线**逐 token 一致**

## 1. 结果总览

| 指标 | openEuler（本次） | busybox（基线，2026-08-21） |
|---|---|---|
| 退出码 | **0** | 0 |
| 判定 | `PASS: W5 inference cluster nodes=4` | 同 |
| range forwards | 16/16 | 16/16 |
| terminal tokens | `[264, 3644, 7653, 304]` | 相同 |
| 生成文本 | `" a global leader in"` | 逐字一致 |
| passed_nodes | 4/4 | 4/4 |
| memory boundary | 12 observations / 12 decisions，status=ok | 同 |
| 稳态单轮 | ~7.2s（barrier ~5.9s 主导） | ~6.3s（barrier ~5.1s） |

PP 几何同为 28 层 4×7 环形（`[0,7) [7,14) [14,21) [21,28)`，nodeD 出 token）。
稳态轮时比 busybox 慢 ~14%：systemd 环境（调度/服务噪声）+ openEuler 内核
tick/调度器差异，量级可接受。

## 2. 运行方式

```bash
cd guest-linux/aarch64
SIM_QWEN3_DENSE_WEIGHTS_PATH=/sd_data/lllm_serving/models/Qwen3-0.6B \
SIM_W5_CLUSTER_NODE_COUNT=4 \
./scripts/run_w5_cluster_qwen3_0_6b_2step_openEuler.sh
# 等价于 SIM_W5_GUEST_ENGINE=openEuler + SIM_W5_OE_DISK_IMAGE=<qcow2> + QEMU_MEM=8G
```

首次运行会用 sudo 从磁盘镜像一次性提取 LVM2 用户态工具到
`/tmp/oe_lvm2_tools`（后续复用）。

## 3. 架构（复用而非重写）

```
每节点 per-run initramfs
├── busybox + LVM2 工具 + UB 模块 + /init=init_switch_root   （openEuler 引导半）
├── 基础 W5 initramfs 解包（staging + 生成的 run_app）        （W5 半，与 busybox 完全同源）
└── /ub_root_overlay/ → 由 init_switch_root 原样部署进真根
    ├── usr/bin/{busybox, linqu_*(23 个), run_app}
    ├── tmp/（对象快照等 staging 载荷）
    └── etc/systemd/system/ub-w5.service（+multi-user.target.wants 软链）
        ExecStart=/bin/busybox sh /bin/run_app，输出走 ttyAMA0 串口
```

宿主侧验证/断言/等待逻辑与 busybox 流程**零分叉**（同一脚本
`run_llm_infer_eight_node_guest.sh` 的 `SIM_W5_GUEST_ENGINE` 分支，仅
initramfs 组装与 QEMU 启动行不同：每节点 qcow2 overlay + `root=...
init=/init enforcing=0`）。

## 4. 调试过程中发现的三个真实坑（均已修复）

1. **busybox `cp -a` 在 usrmerge 下静默丢目录**：目标是符号链接目录
   （`/bin → usr/bin`）时，源目录整体被跳过——overlay 的 bin/ 全没部署。
   修复：`init_switch_root` 逐顶层条目部署，遇符号链接先 `readlink` 解析
   真实目标再拷入。
2. **openEuler 镜像 SELinux=enforcing**：initramfs 写入的文件无安全标签，
   systemd exec 被 AVC 静默拒绝（串口毫无输出）。修复：OE 启动参数加
   `enforcing=0`。
3. **既有 bug**：`run-openEuler-simulated-super-node.sh` 的 `$BUSYBOX`
   未定义（nounset 崩溃），补默认值。

诊断方法沉淀：构造最小 initramfs（skeleton + 只打 marker 的 run_app）单节点
启动 + 挂载 overlay qcow2 直接检查落盘内容——两轮即定位坑 1/2。

## 5. 测试证据

- 新增契约测试 `test_w5_guest_engine_openEuler.py`：8/8 绿（引擎校验、
  overlay/单元生成、usrmerge 解析、permissive 启动参数、非法引擎值拒绝）
- 全量 guest 套件：274 项，仅 3 个**预先存在**的环境 error
  （`/private/tmp` macOS 路径，历史基线已实证）
- 真实运行：4 节点 4-step 全绿（上文数据）

## 6. 遗留与建议

- **QEMU 二进制待重建**：新 qemu pin `3a247cb` 含 OBMM async 设备模型
  （`hw/ub/ub_obmm_async.c`），当前二进制仍是 6 月构建；本次 W5 PP 流程
  未用 async 特性故不受影响。跑 obmm_async 相关流程前需
  `build_qemu_binary.sh` 重建（约 20–30 分钟）。
- openEuler 稳态轮时比 busybox 慢 ~14%，主因仍在跨节点 barrier；
  如需性能对比实验，两引擎已可同参数 A/B。
- `run-openEuler-simulated-super-node.sh` 内联的 LVM2/boot 骨架逻辑与
  `qemu_ub_common.sh` 新共享函数存在重复，后续可收敛（本次未动，避免
  波及独立流程）。

## 7. 产物路径

- 运行日志：`guest-linux/aarch64/logs/2026-08-22_10-17-41_w5_qwen3_0_6b_decode_1125_headless8/`
- 汇总：`guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-08-22_10-17-41_w5_qwen3_0_6b_decode_1125.txt`
- 设计文档：`docs/plans/2026-08-22-w5-openEuler-integration-design.md`
- 主日志留档：`/tmp/w5_oe_v4.log`
- 提交：`cbe21a8`（9 文件 +503/−4）
