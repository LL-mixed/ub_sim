# openEuler demo 使用约定

Console 目录中的 19 个 demo 使用注册目标的 openEuler 磁盘镜像。界面分别显示
执行目标和 Guest OS；历史运行没有 OS 元数据时显示 unknown，不根据当前配置
补写历史结果。目标未注册磁盘镜像或镜像不存在时，启动前返回缺失原因。

覆盖范围：2/4/8 节点 OBMM Pool、URMA RPC、GSVA identity，双节点 Memory Service
和 Direct EL0 Upcall，以及 Qwen3-0.6B、DeepSeek-V4-Flash 的 2/3/4/8 节点
managed serving。

## 远端执行

构建、QEMU、模型和完整测试均在 Linux AArch64 验证目标执行。本地开发机只编辑
和同步源码。以下命令在远端 checkout 中运行，磁盘路径是目标环境提供的路径。

```sh
guest-linux/aarch64/scripts/run_ub_demo.sh \
  --guest-engine openEuler \
  --open-euler-disk-image /path/to/rootfs.qcow2 \
  -- run_ub_four_node_rpc_matrix.sh
```

模型使用既有 W5 配置入口，统一 CLI 会把磁盘选择交给该入口：

```sh
guest-linux/aarch64/scripts/run_ub_demo.sh \
  --guest-engine openEuler \
  --open-euler-disk-image /path/to/rootfs.qcow2 \
  -- run_w5_cluster_config.sh --managed-serving \
  --model /path/to/Qwen3-0.6B --nodes 8 --steps 2 w5.macos.env
```

兼容旧 initramfs userspace：

```sh
guest-linux/aarch64/scripts/run_ub_demo.sh \
  --guest-engine initramfs -- run_ub_dual_node_rpc.sh
```

显式选择 initramfs 会清除继承的 openEuler 磁盘选择。CLI 只接受已审查的 demo
入口名，并保持原参数边界；不接受任意路径或 shell 命令。

运行配置可以放在既有 `.env` 文件中，通过 `set -a; source runtime.env; set +a`
加载。通用 launcher 读取 `SIM_GUEST_ENGINE` 和 `SIM_GUEST_DISK_IMAGE`；W5 保留
其原有 engine 变量和磁盘 CLI。不要拼接多项环境变量赋值来执行 demo。

## 磁盘与应用生命周期

每次启动从当前构建的 initramfs 部署应用、模块和配置，经 `init_switch_root`
切换到 openEuler。通用 demo 由 `linqu-w5-guest.service` 执行原有 `run_app`；W5
继续使用其既有部署流程。基础磁盘不嵌入当前 demo，应用更新不要求重做基础磁盘。

每个节点拥有新建的 qcow2 overlay。自动验收结束、启动失败和 Stop 都先结束该次
guest，再删除它拥有的 overlay 与生成的 boot image。交互模式验证成功后保持
guest 和串口可用，直到 Stop。共享 overlay 父目录中的其他文件不属于该次运行。
boot 准备阶段失败时，公共 helper 显式回收尚未启动 guest 的临时目录；损坏
initramfs 的测试覆盖公共入口及四、八节点 launcher。

当前 launcher 在 kernel cmdline 中使用 `enforcing=0`，让 simulator 的部署应用
和自定义模块能够在原本启用 SELinux enforcing 的基础镜像中运行。

旧八节点 shell 入口 `boot_ub_eight_node_shell.sh` 继续支持 up/status/attach/stop。
其 Docker 启动使用独立 argv 传递 `QEMU_MEM`、`QEMU_SMP`、`APPEND_EXTRA` 和 guest
目录。默认每节点 8 GiB、4 vCPU、`pmd_mapping=100%`、`obmm.mempool_size=0`。

## 构建与验证

标准 `build_guest_artifacts.sh` 必须同时构建模块化 netfs、fscache、9pnet、
9pnet_virtio、9p，保证 modpost 使用完整依赖图。配置为模块但产物缺失时构建
立即失败。virtio block、ext4 与 9p 的配置和产物必须匹配；内置配置无需复制模块。

本次 Direct EL0 修复同时涉及 QEMU device、guest driver 和 EL0 runtime。
在远端加载目标 `.env` 配置后，通过项目 wrapper 重建相关产物：

```sh
guest-linux/aarch64/scripts/build_qemu_binary.sh --with-obmm-tests
guest-linux/aarch64/scripts/build_guest_artifacts.sh
```

QEMU 修复与主仓库的 guest driver、EL0 runtime 必须使用匹配版本；主仓库
gitlink 记录对应的 QEMU 提交。跨设备同步时先同步 QEMU 提交，再同步引用它的
主仓库提交，保证目标环境能够取得完整版本。

Direct EL0 Upcall 显式使用普通 scalar async load，并检查
`direct_el0_upcalls > 0` 与 `kernel_hotpath_ioctls=0`。SVC 上下文恢复和 IRQ 唤醒仍
经过 guest kernel；这一结果不代表整个执行链路不经过内核。

模型验收需逐节点匹配 guest request 与 QEMU 的 typed reads/writes，检查
`shared_payload_staging_bytes=0`，并确认对象、映射、引用和在途请求清理为零。
没有可观察的对应计数时，不根据机制推断更广泛的“零复制”结论。

验证结果及命令见同日期的 openEuler demo 验证报告。生成的日志、磁盘与报告数据
保留在 `out/`、`logs/`，不提交。
