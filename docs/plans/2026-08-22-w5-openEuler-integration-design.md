# W5 on openEuler 集成设计（4 节点 qwen3 PP）

- 日期：2026-08-22
- 状态：机制已实证，方案定稿
- 目标：openEuler guest 上跑通 4 节点 W5 qwen3-0.6B 4-step PP，验证逻辑与 busybox 流程零分叉

## 1. 已实证的机制（Task 11 结论）

- openEuler 流程与 W5 共享：同一 kernel_ub `out/Image`（UB 模块 built-in，设备 00001
  bind 成功）、FM 拓扑（QEMU 级）、串口日志捕获（`nodeX_guest.log`）、
  `linqu_ipourma_ipv4=` cmdline 编址
- 启动链：小 initramfs（busybox+LVM2 工具+模块）→ `init_switch_root`：
  挂 proc/sys/dev → 绑定 UB 设备 → 激活 LVM → 挂 openEuler ext4 真根（每节点
  qcow2 overlay，基础镜像零修改）→ 部署 `/ub_apps/*` → `/opt/ub_sim/` →
  switch_root 交给 systemd
- **镜像侧无任何 app 自动化**（无 ub_sim systemd 单元）→ 注入杠杆在
  initramfs 侧（switch_root 前对 rw 真根任意写）
- 本机已修复 openEuler 脚本既有 bug：`BUSYBOX` 未定义（nounset 崩溃）
- 2 节点试跑到 `localhost login:` 全绿

## 2. 核心设计：复用生成的 run_app，验证零分叉

**不重写 W5 guest 侧逻辑。** busybox 流程的 `write_w4_initramfs_runner()`
生成的 `run_app`（内含全部 env 导出 + 校验 + `exec /bin/linqu_init`）原样
生成一份，通过 rootfs 覆盖层放进 openEuler：

```
per-node initramfs（现有 build_node_initramfs 扩展）
├── busybox + LVM2 工具 + 模块           （已有）
├── /ub_apps/                            （已有：静态 app → /opt/ub_sim）
└── /ub_root_overlay/                    （新增：原样 cp -a 到 /newroot/）
    ├── bin/busybox                      （run_app 内部用 /bin/busybox cat 等）
    ├── bin/linqu_init, linqu_mem_service, linqu_llm_infer ...
    ├── opt/ub_sim/run_app               （生成的 W5 runner 原样）
    └── etc/systemd/system/ub-w5.service（oneshot 执行器）
        └── multi-user.target.wants/ub-w5.service → ../ub-w5.service
```

`ub-w5.service`（模板，宿主生成）：

```ini
[Unit]
Description=UB SIM W5 guest runner
After=multi-user.target
[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=0
ExecStartPre=-/usr/bin/systemctl stop firewalld
StandardOutput=tty
TTYPath=/dev/ttyAMA0
ExecStart=/bin/busybox sh /opt/ub_sim/run_app
[Install]
WantedBy=multi-user.target
```

输出走 `/dev/ttyAMA0` = 串口 logfile → **宿主侧 wait/assert 全套原样复用**
（marker 是行级正则，systemd 启动噪声不影响 `rg` 匹配）。

## 3. 宿主侧：引擎分支，不是新脚本

`run_llm_infer_eight_node_guest.sh` 加 `SIM_W5_GUEST_ENGINE=openEuler` 分支：

| 环节 | busybox 路径（现状） | openEuler 分支 |
|---|---|---|
| initramfs | build_w4_initramfs（单一 run_app） | 逐节点 build（busybox+LVM2+模块+**同一 run_app**+overlay 树），复用 write_w4_initramfs_runner 生成 |
| 启动 | initramfs-only QEMU | + `-drive overlay.qcow2`（基于 `SIM_W5_OE_DISK_IMAGE`） |
| 其余（FM/QMP/串口/等待/断言/cleanup） | 不变 | 不变 |

新入口 `run_w5_cluster_qwen3_0_6b_2step_openEuler.sh`：镜像 2step 包装器，
追加 `SIM_W5_GUEST_ENGINE=openEuler` + `SIM_W5_OE_DISK_IMAGE` 默认值 +
`QEMU_MEM=8G`。

## 4. 风险与对策（实证项）

| 风险 | 对策 |
|---|---|
| firewalld 拦 10.0.0.x | unit ExecStartPre stop firewalld（试跑日志确认它在跑） |
| NetworkManager 干扰 ipourma | 试跑日志中 NM 未启动（0 命中）；run_app 自带 iface 配置；首跑观察 |
| systemd 启动慢（~40s vs busybox ~3s） | APP_WAIT_SECS/gate 等待沿用（本就按分钟计） |
| usrmerge（/bin→/usr/bin） | overlay 写 /newroot/bin 即 /usr/bin，路径解析不受影响 |
| init.c（linqu_init）内部依赖 | Task 13 首步通读 init.c 的路径假设（/proc、cmdline、app 路径） |

## 5. 实施步骤

1. `init_switch_root`：新增 `/ub_root_overlay` 原样覆盖部署（~15 行）
2. `run_llm_infer_eight_node_guest.sh`：引擎分支（openEuler initramfs 构建 +
   overlay 启动 + systemd unit 生成）
3. `run_w5_cluster_qwen3_0_6b_2step_openEuler.sh` 入口 + AGENTS.md 文档
4. 契约测试：engine 分支/overlay 部署/unit 生成的源级断言（沿用现有测试风格）
5. 2 节点冒烟 → 4 节点 4-step PP 全量 → 与 busybox 基线对比 token 输出
6. 报告 + 提交（含 BUSYBOX 修复）
