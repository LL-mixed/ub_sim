# UB Data System Simulator

这个仓库是从零把 UB/Linqu 数据系统仿真环境拉起来的 meta repo，使用本 repo 中的 artifacts，可以稳定地模拟 UB full-mesh 互联的2节点、4 节点、8 节点运行环境。

按下面顺序做：

1. clone 仓库并初始化 submodule
2. 准备 guest kernel artifact 和 ARM64 busybox
3. 让脚本自动构建 QEMU + initramfs
4. 启动双节点 / 4 节点 / 8 节点
5. 自动化验证使用 headless 模式；人工调试时可通过 tmux 或串口端口与 guest 交互

## 目录说明

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
- `vendor/simpler`
- `vendor/pto-isa`

如果你只想单独刷新某个 submodule：

```bash
git submodule update --init vendor/qemu_8.2.0_ub guest-linux/kernel_ub vendor/simpler vendor/pto-isa
```

## 2. 环境要求

宿主机至少需要这些命令：

- `zsh`
- `python3`
- `tmux`
- `nc`
- `rg`
- `ninja`
- `cmake`
- host C/C++ 编译器；在 `a2a3sim` simpler kernel 路径上还需要 `g++-15`
- ARM64 Linux 交叉编译器，匹配 `aarch64-*-gnu-gcc`

还需要两类 guest 输入：

- ARM64 Linux kernel artifact
  包括 `Image` 和 guest 需要加载的 `.ko` 模块
- ARM64 静态 busybox
  用来生成 initramfs 和交互 shell

这套脚本的设计原则是：

- QEMU 二进制缺失时自动构建
- `out/Image` 或 `out/initramfs.cpio.gz` 过期时自动刷新
- 已经新鲜的产物会复用，不会每次全量重建

自动化运行约定：

- autotest、demo validation、matrix harness、CI 回归都必须使用 headless 启动/控制路径。
- `tmux` launcher 只用于人工交互、串口观察和临时 debug，不作为 harness control plane。
- 如果某个验证脚本仍依赖 tmux，它应先迁移到 headless，再纳入自动化回归。

## 3. 准备 Guest Artifact

所有 guest 相关脚本默认都在 [guest-linux/aarch64](guest-linux/aarch64) 下工作。最重要的环境变量是：

- `AARCH64_LINUX_CC`
  ARM64 Linux 交叉编译器
- `BUSYBOX`
  ARM64 静态 busybox 可执行文件

推荐先设置：

```bash
export AARCH64_LINUX_CC=/path/to/aarch64-*-gnu-gcc
export BUSYBOX=$PWD/guest-linux/aarch64/busybox-aarch64
```

`BUSYBOX` 指向的是 ARM64 静态 busybox 可执行文件，不是源码目录。
脚本会按下面顺序找它：

1. 显式传入的 `BUSYBOX`
2. `guest-linux/aarch64/busybox-aarch64`
3. `guest-linux/aarch64/third_party/busybox-aarch64`
4. `guest-linux/aarch64/third_party/busybox-src`
5. `guest-linux/aarch64/third_party/busybox-*.tar.bz2`

也就是说，如果你不想每次 export `BUSYBOX`，最简单的做法就是把最终二进制放在：

```bash
guest-linux/aarch64/busybox-aarch64
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

### 3.2 从显式 remote Linux build host 同步 kernel artifact

如果你的 kernel 构建发生在远端 Linux build host/build farm，必须显式提供 SSH 目标并开启 remote artifact 同步：

```bash
cd guest-linux/aarch64
ARTIFACT_SOURCE=remote \
ALLOW_REMOTE_LINUX_ARTIFACTS=1 \
REMOTE_LINUX_HOST=<your-build-host> \
REMOTE_KERNEL_SRC=/path/to/kernel_ub \
REMOTE_KERNEL_BUILD=/path/to/kernel_build \
REMOTE_TMPDIR=/path/to/writable/tmp \
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/build_guest_artifacts.sh
```

其中 `REMOTE_LINUX_HOST` 是你自己的 SSH 目标，例如 `user@build-host`。

如果远端根分区可用空间不足（VM 经常会出现），请把 `REMOTE_KERNEL_SRC` / `REMOTE_KERNEL_BUILD` / `REMOTE_TMPDIR` 放在有空间的挂载点（例如 `/mnt/share/...`），否则容易在 kernel 编译中触发 `No space left on device`。

当前仓库在 `ll@192.168.64.3` VM 上已有可用内核构建环境，可直接用如下参数：

```bash
cd guest-linux/aarch64
export UB_REMOTE_LINUX_HOST=ll@192.168.64.3
export UB_ALLOW_REMOTE_LINUX_ARTIFACTS=1
export UB_REMOTE_KERNEL_SRC=/home/ll/ub_sim/guest-linux/kernel_ub
export UB_REMOTE_KERNEL_BUILD=/mnt/share/shared_data/ub_sim_kernel_build
export UB_REMOTE_TMPDIR=/mnt/share/shared_data/ub_sim_kernel_tmp
export UB_GUEST_ARTIFACT_SOURCE=remote
export UB_SYNC_ARTIFACTS=1

ARTIFACT_SOURCE=remote \
AARCH64_LINUX_CC=/opt/homebrew/bin/aarch64-linux-gnu-gcc \
BUSYBOX=$PWD/busybox-aarch64 \
./scripts/build_guest_artifacts.sh
```

如果你希望同步脚本在 `run_ub_*.sh` 链路里自动生效，只需把上述 `UB_*` 环境变量导出后直接运行对应启动脚本，`ensure_ub_guest_artifacts` 会在内部透传到 `build_guest_artifacts.sh`。

默认 `ARTIFACT_SOURCE=auto`，行为是：

- 先复用本地新鲜的 `out/` 产物
- 本地产物过期时，优先导入 `LOCAL_KERNEL_IMAGE` / `LOCAL_MODULES_DIR`
- 如果当前环境是 Linux 且有 `aarch64-*-gnu-gcc`，走本机 native cross build
- native guest kernel 目标架构是 `arm64`，默认使用 `openeuler_defconfig` 作为 defconfig，再叠加 UB demo/harness 需要的 kernel config
- 没有本地导入参数且无法 native cross build 时失败并提示；不会自动访问 remote Linux build host

### 3.3 准备 simpler HostBuildGraph Artifact

W4 workload 的 `host_vector`、`host_matmul`、`qwen3_dense_0_6b` profile 需要 simpler HostBuildGraph
artifact。新环境里不要依赖已有的 `/tmp/simpler-host-*` 缓存，本 repo 会从 `vendor/simpler` 和
`vendor/pto-isa` 构造这些产物。

手工生成命令：

```bash
cd guest-linux/aarch64
./scripts/prepare_simpler_host_vector_artifacts.sh /tmp/simpler-host-vector-artifacts
./scripts/prepare_simpler_host_matmul_artifacts.sh /tmp/simpler-host-matmul-artifacts
```

也可以使用统一 Python CLI：

```bash
cd guest-linux/aarch64
./scripts/prepare_simpler_host_artifacts.py --profile host_vector --output-dir /tmp/simpler-host-vector-artifacts
./scripts/prepare_simpler_host_artifacts.py --profile host_matmul --output-dir /tmp/simpler-host-matmul-artifacts
```

HostMatmul producer 支持生成 batched tile artifact。为了避免同一进程反复加载不同 simpler runtime，
`--tile-batch > 1` 必须显式复用基础 manifest 的 runtime：

```bash
./scripts/prepare_simpler_host_artifacts.py \
  --profile host_matmul \
  --output-dir /tmp/simpler-host-matmul-batch2-artifacts \
  --tile-batch 2 \
  --reuse-runtime-manifest /tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json
```

`sim-cli qwen3-decode-loop` 可以直接接收 batched matmul 配置。`--matmul-batch N` 会设置
decode dispatch batch，并在默认 batch manifest 缺失时自动生成对应 artifact：

```bash
SIM_QWEN3_0_6B_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B \
cargo run --release -p sim-cli -- qwen3-decode-loop \
  --scenario 2host \
  --max-token 32 \
  --prompt "Capital of China is" \
  --matmul-batch 4
```

`--prompt` 默认会按 Qwen3 chat template 包装成 user/assistant 消息；需要完全按原始文本
tokenize 时传 `--raw-prompt`。

`--scenario` 可用 `2host`、`4host`、`8host`，也可以传完整 YAML 路径。这个参数同时用于
`sim-cli` 外层 topology 和 `sim-uapi` 内层 chipbackend runtime，避免两边读取不同 scenario。

4/8 node headless launcher 和 W4 run 脚本在 manifest 缺失时会自动调用对应 producer。默认 manifest：

- `SIMPLER_HOST_VECTOR_MANIFEST=/tmp/simpler-host-vector-artifacts/host_vector_manifest.json`
- `SIMPLER_HOST_MATMUL_MANIFEST=/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json`

如果你把 artifact 放在别处，显式传这两个环境变量即可。

### 3.4 只准备 busybox

如果你还没有 busybox，可以让脚本在 `guest-linux/aarch64` 下自动准备：

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" ./scripts/prepare_busybox.sh
```

这个脚本的目标是生成可直接被 initramfs 使用的 `busybox-aarch64`。准备完成后，推荐：

```bash
export BUSYBOX=$PWD/busybox-aarch64
```

如果你已经把 `busybox-aarch64` 放在 `guest-linux/aarch64/` 下，后续执行：

```bash
./scripts/build_guest_artifacts.sh
./scripts/launch_ub_dual_node_tmux.sh
```

即使不再显式传 `BUSYBOX`，脚本也会自动复用它。

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
/bin/run_demo udma
/bin/run_demo obmm_pool
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

### 5.3 双节点 headless demo 验证

如果你要直接跑双节点 demo 验证，而不是先手动进 shell，使用无交互/headless harness：

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
- UAPI scenario: `scenarios/mvp_4host_single_domain.yaml`
- 节点：`nodeA/nodeB/nodeC/nodeD`

和双节点不同的是，四节点脚本会等所有 guest 都完成 `/bin/run_demo` bootstrap，再报告 shell ready。

### 6.2 在四节点环境里交互

和双节点一样，直接在各个 `nodeX-guest` 窗口里操作。

常用命令仍然是：

```bash
/bin/run_demo chat
/bin/run_demo rpc
/bin/run_demo udma
/bin/run_demo obmm_pool
/bin/run_demo shell
```

### 6.3 四节点验证脚本

四节点 autotest/demo/matrix 验证应走 headless harness，不应通过
`launch_ub_four_node_tmux.sh` 作为控制平面。tmux 仅用于上一节的人工交互环境。

四节点常用脚本：

```bash
cd guest-linux/aarch64
./scripts/run_ub_four_node_smoke.sh
./scripts/run_ub_four_node_chat_matrix.sh
./scripts/run_ub_four_node_rpc_matrix.sh
./scripts/run_ub_four_node_udma_matrix.sh
./scripts/run_ub_four_node_obmm_pool.sh
```

`run_ub_four_node_obmm_pool.sh` 默认使用每节点 `4 vCPU + 8G`、`pmd_mapping=100%`、
`obmm.mempool_size=0`，并导出 `7680MB` OBMM shmem pool。这个配置用于验证
4-node full-mesh 下每节点 7.5GB export/import 和 payload round-trip。

这些脚本会生成各自 report，并把详细日志落到 `logs/` 和 `out/`。用于自动化时，日志目录名应带
`headless4`/`headless8` 这类 headless run id，而不是 `tmux` run id。

## 7. 八节点

当前仓库里八节点主路径是 headless，不是 tmux 交互优先。八节点 autotest、demo validation
和 matrix harness 都必须保持 headless。

### 7.1 启动八节点 headless 环境

```bash
cd guest-linux/aarch64
AARCH64_LINUX_CC="$AARCH64_LINUX_CC" \
BUSYBOX="$BUSYBOX" \
./scripts/launch_ub_eight_node_headless.sh
```

它会打印一个环境文件路径，例如：

- `out/headless_eight_node_env.<run_id>.sh`

默认使用：

- topology: `vendor/ub_topology_eight_node_full_mesh.ini`
- UAPI scenario: `scenarios/mvp_8host_single_domain.yaml`

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
./scripts/run_ub_eight_node_udma_matrix.sh
./scripts/run_ub_eight_node_obmm_pool.sh
```

其中 8 节点会默认设置 `UB_SIM_PORT_NUM=7`，对应 full-mesh 端口数。
所有 8 节点 headless workload 默认使用 `QEMU_MEM=6G` 和
`pmd_mapping=30%`；该组合会在 guest 内为 `pfn_range_alloc` 预留 1 GiB，
避免 `4G + pmd_mapping=25%/50%` 下的 OBMM contiguous allocation failure。
需要时仍可通过 `QEMU_MEM` 和 `APPEND_EXTRA` 覆盖。

## 8. 启动 openEuler simulated super-node

除了前面几节的轻量 busybox guest，仓库也支持让每个 QEMU node 从 openEuler qcow2
磁盘镜像启动完整 Linux 发行版。入口脚本是：

- `guest-linux/aarch64/scripts/run-openEuler-simulated-super-node.sh`

它会为每个 node：

1. 基于原始 openEuler 磁盘创建独立的 qcow2 overlay（不修改原盘）
2. 从镜像中提取 aarch64 LVM2 工具/库，打包进每个 node 的 initramfs
3. 在 initramfs 中激活 LVM、挂载 rootfs，并注释掉会导致 emergency mode 的 `/boot/efi` 和 swap 条目
4. 将 `--app-dir` 中的应用复制到 `/opt/ub_sim/`
5. `switch_root` 到 openEuler 的 `/sbin/init`

### 8.1 前置条件

- 一个可启动的 openEuler ARM64 qcow2 磁盘镜像（例如 `~/vms/openEuler-2403/disk.qcow2`）
- 已构建好的 guest kernel `guest-linux/aarch64/out/Image`
- 该 kernel 配置已包含 openEuler 所需驱动：`VIRTIO_BLK/PCI/NET`、`EXT4_FS`、`BLK_DEV_DM` 等
- ARM64 静态 busybox

### 8.2 启动 2 节点

```bash
cd guest-linux/aarch64
export AARCH64_LINUX_CC=aarch64-linux-gnu-gcc
export BUSYBOX=$PWD/busybox-aarch64

./scripts/run-openEuler-simulated-super-node.sh \
  --disk ~/vms/openEuler-2403/disk.qcow2 \
  --nodes 2 \
  --memory 4G \
  --smp 2
```

### 8.3 带应用目录启动

```bash
./scripts/run-openEuler-simulated-super-node.sh \
  --disk ~/vms/openEuler-2403/disk.qcow2 \
  --nodes 4 \
  --memory 8G \
  --smp 4 \
  --app-dir ./my_apps \
  --demo gsva_identity
```

### 8.4 常用参数

| 参数 | 说明 |
|---|---|
| `--disk PATH` | 原始 openEuler qcow2 磁盘镜像（必需） |
| `--nodes N` | 节点数：2 / 4 / 8（默认 2） |
| `--topology FILE` | UB topology ini 文件 |
| `--memory SIZE` | 每节点内存，默认 4G |
| `--smp N` | 每节点 vCPU 数，默认 4 |
| `--app-dir DIR` | 要部署到每个节点 `/opt/ub_sim/` 的应用目录 |
| `--demo MODE` | demo 模式提示 |
| `--out-dir DIR` | 输出目录，默认 `out/openEuler-super-node` |
| `--run-id ID` | 自定义 run-id |

### 8.5 清理

脚本会打印 cleanup 脚本路径，运行即可停止所有 node：

```bash
/tmp/oe_super_test/nodes/<run_id>/cleanup.sh
```

### 8.6 注意事项

- 部分 UB `.ko` 可能报 `disagrees about version of symbol module_layout`，但对应驱动已 built-in，不影响启动和 UB 功能。
- 原始 qcow2 不会被修改，每个节点使用独立的 overlay。
- 如果 guest 内有其他依赖特定物理分区的服务失败，可在 initramfs 阶段进一步扩展 `/etc/fstab` 清理逻辑。

## 9. 常见交互和产物位置

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

## 10. 进一步文档

- [guest-linux/aarch64/README.md](guest-linux/aarch64/README.md)
  更细的 guest harness、run_demo、tmux 细节
- [docs/README.md](docs/README.md)
  设计说明和验证报告入口
- [scenarios/README.md](scenarios/README.md)
  scenario 输入说明

## 11. 对用户的实际影响

这版 README 关注的是“从零启动到可交互”的完整路径，而不是继续做仓库索引。这样新机器、新同事、未来的自己回来看时，不需要先读一堆历史材料再猜命令入口，直接按步骤执行就能知道：

- 该 clone 什么
- 该准备什么环境变量
- 该如何生成产物
- 该跑哪个脚本拉起不同规模节点
- 拉起后怎么进入 guest 和怎么清理
