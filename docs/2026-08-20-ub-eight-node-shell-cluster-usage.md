# 8 节点 UB 互联 openEuler 交互式集群 — 用法速查

**日期：** 2026-08-20
**入口脚本：** `guest-linux/aarch64/scripts/boot_ub_eight_node_shell.sh`（commit `9f68e95`）
**运行位置：** n4-910c（Linux aarch64 验证机），不要在本地 macOS 跑

## 命令速查

```bash
ssh n4-910c
cd /home/ll/ub_sim

S=./guest-linux/aarch64/scripts/boot_ub_eight_node_shell.sh

$S up            # 拉起 8 节点集群（约 1-2 分钟；guest 内 systemd 引导 30-60s）
$S attach nodeA  # 进入 nodeA 的 root shell（nodeA..nodeH；按 Enter 出提示符，Ctrl-] 退出）
$S status        # 查看 8 个节点 socket 状态
$S stop          # 销毁集群（杀 QEMU、删容器、清 socket）
```

节点标识：hostname = `ub-node-1`..`ub-node-8`（对应 nodeA..nodeH），UB IP = `10.0.0.1`..`10.0.0.8`。
guest 内可见 UB 设备：`/dev/ub_npu0`、`/dev/ub_ssd0`、`/dev/ubagg`；9p 共享挂载在 `/run/w5/{env,tools,modules}`。

## 工作机制（排障时用）

- 复用 openEuler 镜像内 `linqu-w5-guest.service` 的注入通道：宿主机把 `runner.sh` 写进
  `out/openeuler_guest/env/`，guest 启动时 systemd 通过 9p（tag `w5env`）挂载到 `/run/w5/env`
  并以 root 执行——shell 模式下 runner.sh 在 ttyAMA0 上起交互式 bash。
- QEMU 跑在长驻 docker 容器 `ub-shell-8node`（镜像 `openeuler-2403:v0.0.4`）里，因为预编译
  QEMU 动态链接容器内的 liburing/pixman（宿主机没有）。串口 socket 目录
  `/tmp/ub-qemu-links-eight` bind-mount 到宿主机，宿主机直接 attach。
- 启动时用 `UB_USE_PREBUILT_QEMU=1 UB_SYNC_ARTIFACTS=0`，复用已验证的 QEMU 与 guest 产物，
  绝不从该流程触发重建。

## 注意事项

1. **W5 测试会覆盖 shell 模式**：W5 流程每次运行重写 `env/runner.sh`。跑过 W5 后再 `$0 up` 一次即恢复。
2. 串口 socket 由容器内 root 创建，`up` 时已自动 `chmod a+rw`；若手工重启过 QEMU 需重做。
3. guest 是极简镜像：无 `hostname(1)` 等工具，改主机名用 `echo NAME > /proc/sys/kernel/hostname`。
4. n4 根分区常年 ~100%：异常时先 `df -h /`，清理 `/var/log/atop`。
5. 每节点 qcow2 overlay 在 `/tmp/ub-qemu-links-eight/overlays/`，`stop` 不删 overlay，内容可复用。

## 验证记录（2026-08-20）

8 节点全部启动；nodeA/nodeH 串口 shell 实测执行命令返回正确（hostname、UB IP、UB 设备、
dmesg 中 UB/OBMM 记录 359 行）。契约测试 `test_boot_ub_eight_node_shell_contract.py` 6/6 通过。
