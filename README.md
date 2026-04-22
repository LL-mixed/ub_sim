# UB Simulator Workspace

这个仓库的目标不是只放代码，而是让你能从零把 UB/Linqu 仿真环境拉起来，并且能稳定地做双节点、4 节点、8 节点验证。

如果你只想最快跑通，按下面顺序做：

1. clone 仓库并初始化 submodule
2. 准备 guest kernel artifact 和 ARM64 busybox
3. 让脚本自动构建 QEMU + initramfs
4. 启动双节点 / 4 节点 / 8 节点
5. 通过 tmux 或串口端口进入 guest 交互

## 目录

- `guest-linux/aarch64/`
  guest 启动、initramfs、demo、QEMU 启动脚本都在这里
- `vendor/qemu_8.2.0_ub/`
  QEMU submodule，脚本会按需构建 `qemu-system-aarch64`
- `guest-linux/kernel_ub/`
  kernel submodule，guest `Image` 和 `.ko` 模块与它对应
- `vendor/*.ini`
  双节点、4 节点、8 节点 topology 文件
- `docs/`
  验证报告、设计文档、历史材料

## 1. Clone 和 Submodule 初始化

```bash
git clone <your-repo-url>
cd ub_sim.git
git submodule update --init --recursive
```

当前关键 submodule：

- `vendor/qemu_8.2.0_ub`
- `guest-linux/kernel_ub`

如果你只想单独刷新某个 submodule：

```bash
git submodule update --init vendor/qemu_8.2.0_ub guest-linux/kernel_ub
```

## 2. 环境要求

宿主机至少需要这些命令：

- `zsh`
- `python3`
- `tmux`
- `nc`
- `rg`
- `ninja`
- ARM64 Linux 交叉编译器，例如 `aarch64-unknown-linux-gnu-gcc`

还需要两类 guest 输入：

- ARM64 Linux kernel artifact
  包括 `Image` 和 guest 需要加载的 `.ko` 模块
- ARM64 静态 busybox
  用来生成 initramfs 和交互 shell

这套脚本的设计原则是：

- QEMU 二进制缺失时自动构建
- `out/Image` 或 `out/initramfs.cpio.gz` 过期时自动刷新
- 已经新鲜的产物会复用，不会每次全量重建

## 3. 准备 Guest Artifact

所有 guest 相关脚本默认都在 [guest-linux/aarch64](guest-linux/aarch64) 下工作。最重要的环境变量是：

- `AARCH64_LINUX_CC`
  ARM64 Linux 交叉编译器
- `BUSYBOX`
  ARM64 静态 busybox 可执行文件

推荐先设置：

```bash
export AARCH64_LINUX_CC=/opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc
export BUSYBOX=$PWD/guest-linux/aarch64/busybox-aarch64
```

### 3.1 本地导入 kernel artifact

如果你已经有现成的 `Image` 和模块目录，这是最直接的方式：

```bash
cd guest-linux/aarch64
ARTIFACT_SOURCE=local \
LOCAL_KERNEL_IMAGE=/path/to/Image \
LOCAL_MODULES_DIR=/path/to/modules \
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/build_guest_artifacts.sh
```

执行结果：

- `out/Image`
- `out/initramfs.cpio.gz`
- `out/modules/*.ko`

会被准备好，后续所有启动脚本默认都用这套产物。

### 3.2 从 VM 同步 kernel artifact

如果你的 kernel 构建发生在远端 VM，脚本支持自动拉取：

```bash
cd guest-linux/aarch64
VM_HOST=ll@192.168.64.3 \
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/build_guest_artifacts.sh
```

默认 `ARTIFACT_SOURCE=auto`，行为是：

- 先复用本地新鲜的 `out/` 产物
- 本地产物过期时，优先导入 `LOCAL_KERNEL_IMAGE` / `LOCAL_MODULES_DIR`
- 没有本地导入参数且 VM 可达时，自动从 VM 同步

### 3.3 只准备 busybox

如果你还没有 busybox，可以让脚本在 `guest-linux/aarch64` 下自动准备：

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" ./scripts/prepare_busybox.sh
```

## 4. 显式构建 QEMU

大部分启动脚本会自动调用 `build_qemu_binary.sh`，所以通常不需要手工先构建。但如果你想提前确认 QEMU 可以单独编过：

```bash
cd guest-linux/aarch64
./scripts/build_qemu_binary.sh
```

产物位置：

- `vendor/qemu_8.2.0_ub/build/qemu-system-aarch64`

## 5. 双节点

### 5.1 启动双节点交互环境

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/launch_ub_dual_node_tmux.sh
```

这个命令会自动：

- 检查并构建 QEMU
- 检查并准备 `out/Image` 和 `out/initramfs.cpio.gz`
- 用 `vendor/ub_topology_two_node_v0.ini` 启动 `nodeA/nodeB`
- 自动 attach 到 tmux session

tmux 里你会看到：

- `control`
- `nodeA-qemu`
- `nodeB-qemu`
- `nodeA-guest`
- `nodeB-guest`

### 5.2 在双节点 guest 中交互

启动完成后，guest 默认会先走 `/bin/run_demo` bootstrap，然后进入 shell。

在 guest 里常用命令：

```bash
/bin/run_demo chat
/bin/run_demo rpc
/bin/run_demo rdma
/bin/run_demo obmm
/bin/run_demo all
/bin/run_demo shell
```

如果你已经退回宿主机，可以重新进入 tmux：

```bash
tmux attach -t <session-name>
```

控制窗口会打印：

- session name
- log 路径
- cleanup 脚本路径

### 5.3 双节点无交互 demo 验证

如果你要直接跑双节点 demo 验证，而不是先手动进 shell：

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/run_ub_dual_node_demo.sh
```

输出会写到：

- `out/demo_report.latest.txt`
- `logs/`

## 6. 四节点

### 6.1 启动四节点交互环境

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/launch_ub_four_node_tmux.sh
```

这个命令会使用：

- topology: `vendor/ub_topology_four_node_full_mesh.ini`
- 节点：`nodeA/nodeB/nodeC/nodeD`

和双节点不同的是，四节点脚本会等所有 guest 都完成 `/bin/run_demo` bootstrap，再报告 shell ready。

### 6.2 在四节点环境里交互

和双节点一样，直接在各个 `nodeX-guest` 窗口里操作。

常用命令仍然是：

```bash
/bin/run_demo chat
/bin/run_demo rpc
/bin/run_demo rdma
/bin/run_demo obmm
/bin/run_demo shell
```

### 6.3 四节点验证脚本

四节点常用脚本：

```bash
cd guest-linux/aarch64
./scripts/run_ub_four_node_smoke.sh
./scripts/run_ub_four_node_chat_matrix.sh
./scripts/run_ub_four_node_rpc_matrix.sh
./scripts/run_ub_four_node_rdma_matrix.sh
./scripts/run_ub_four_node_obmm_pool.sh
```

这些脚本会生成各自 report，并把详细日志落到 `logs/` 和 `out/`。

## 7. 八节点

当前仓库里八节点主路径是 headless，不是 tmux 交互优先。

### 7.1 启动八节点 headless 环境

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/launch_ub_eight_node_headless.sh
```

它会打印一个环境文件路径，例如：

- `out/headless_eight_node_env.<run_id>.sh`

这个文件里会导出：

- `RUN_DIR`
- `CLEANUP_SCRIPT`
- `NODEA_SERIAL_PORT` 到 `NODEH_SERIAL_PORT`

### 7.2 与八节点交互

先 source 环境文件：

```bash
source guest-linux/aarch64/out/headless_eight_node_env.<run_id>.sh
```

然后通过串口端口接入某个 guest：

```bash
nc 127.0.0.1 "$NODEA_SERIAL_PORT"
nc 127.0.0.1 "$NODEH_SERIAL_PORT"
```

日志目录在：

- `$RUN_DIR`

清理环境：

```bash
"$CLEANUP_SCRIPT"
```

### 7.3 八节点验证脚本

```bash
cd guest-linux/aarch64
./scripts/run_ub_eight_node_smoke.sh
./scripts/run_ub_eight_node_chat_matrix.sh
./scripts/run_ub_eight_node_rpc_matrix.sh
./scripts/run_ub_eight_node_rdma_matrix.sh
./scripts/run_ub_eight_node_obmm_pool.sh
```

其中 8 节点会默认设置 `UB_SIM_PORT_NUM=7`，对应 full-mesh 端口数。

## 8. 常见交互和产物位置

### 日志

- `guest-linux/aarch64/logs/`
- `guest-linux/aarch64/out/`

### 常见文件

- kernel: `guest-linux/aarch64/out/Image`
- initramfs: `guest-linux/aarch64/out/initramfs.cpio.gz`
- modules: `guest-linux/aarch64/out/modules/`
- report: `guest-linux/aarch64/out/*.latest.txt`
- cleanup script: `guest-linux/aarch64/out/*cleanup*.sh`

### 清理运行中的节点

启动脚本都会打印 cleanup 脚本路径。优先用它清理，不要自己手工找 PID。

## 9. 进一步文档

- [guest-linux/aarch64/README.md](guest-linux/aarch64/README.md)
  更细的 guest harness、run_demo、tmux 细节
- [docs/README.md](docs/README.md)
  设计说明和验证报告入口
- [scenarios/README.md](scenarios/README.md)
  scenario 输入说明

## 10. 对用户的实际影响

这版 README 关注的是“从零启动到可交互”的完整路径，而不是继续做仓库索引。这样新机器、新同事、未来的自己回来看时，不需要先读一堆历史材料再猜命令入口，直接按步骤执行就能知道：

- 该 clone 什么
- 该准备什么环境变量
- 该如何生成产物
- 该跑哪个脚本拉起不同规模节点
- 拉起后怎么进入 guest 和怎么清理
