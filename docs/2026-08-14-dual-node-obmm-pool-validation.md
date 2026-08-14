# 2026-08-14 Dual-Node OBMM Pool Validation (macOS Host + n4-910c Build)

## 结论

**PASS** — 双节点 OBMM pool demo 全链路通过：export / import / 所有权轮转 / payload 校验 / stress / 清理全部 ok。

## 运行环境

| 项 | 值 |
|---|---|
| Host（运行 QEMU） | macOS (Darwin 25.5.0)，本地构建 `vendor/qemu_8.2.0_ub/build/qemu-system-aarch64` |
| 构建机 | n4-910c（远程 aarch64 Linux，320 核），kernel/native gcc 10.3.1 构建 |
| Guest kernel | Linux 6.6.0+（kernel_ub @ `92d6c59`，SMP #1 2026-08-14 12:08 CST） |
| 拓扑 | `vendor/ub_topology_two_node_v0.ini`，2 节点 full-mesh（nodeA=10.0.0.1, nodeB=10.0.0.2） |
| 每节点配置 | 1 vCPU / 8G RAM，`pmd_mapping=25%`，`obmm.skip_cache_maintain=1` |
| run_id | `2026-08-14_12-14-10_obmm_pool_16768` |

## 测试参数

- 每节点 export：**512MB**（cluster 聚合 1024MB）
- import cache mode：`auto`（shmdev mmap mode=osync）
- stress：20 iterations，1 remote slot
- round timeout：60s

## 关键结果

| 阶段 | nodeA | nodeB |
|---|---|---|
| export (512MB) | ok mem_id=1 uba=0xffffe0000000 token=96 | ok mem_id=1 uba=0xffffe0000000 token=96 |
| metadata exchange | ok count=2 | ok count=2 |
| import peer | ok owner=2 mem_id=2 local_pa=0x40000000000 | ok owner=1 mem_id=2 local_pa=0x40000000000 |
| pool ready | ok nodes=2 aggregate=1024MB | ok nodes=2 aggregate=1024MB |
| 所有权轮转 rounds | ok count=2（owner 1→2，ACK/COMMIT 协议完整） | ok count=2 |
| payload verify | 两轮 owner 写入后对端 verify 均 ok | 同左 |
| stress 20 iters | 45ms, **27.8 MB/s** | 43ms, **29.1 MB/s** |
| stress barrier | ok count=2 | ok count=2 |
| unexport/unimport | ok | ok |

mem_window decode：MAR0 `[0x40000000000,+2G]`、MAR3 `[0x48000000000,+2G]`、MAR4 `[0x50000000000,+4G]`，cc/nc 窗口正常建立。

## 环境搭建说明（本次新增）

本机首次从零跑通，依赖项处理记录：

1. **QEMU (macOS)**：`build_qemu_binary.sh` 原生构建；需 `ninja`（brew 安装）和 `distlib`（`/usr/bin/python3 -m pip install --user distlib`，QEMU mkvenv 需要）。
2. **busybox**：直接复用 n4-910c 上已有的静态 `busybox-aarch64`，放在 `guest-linux/aarch64/busybox-aarch64`。
3. **aarch64 Linux 交叉编译器（macOS）**：`brew tap messense/macos-cross-toolchains && brew install aarch64-unknown-linux-gnu`（brew trust 该 tap 后可装），用于本地构建 initramfs。
4. **n4-910c 远端 kernel 构建**：
   - 根分区 100% 满，无法 `dnf install`；flex/bison 从源码装到 `~/tools` 并软链到 `~/.local/bin`。
   - openssl-devel 缺失：openssl-1.1.1w 头文件装到 `/usr/local/include`，`libcrypto.so` 软链到 `/usr/lib64`（指向系统 `libcrypto.so.1.1`）。
   - 因 openeuler_defconfig 的 `SECURITY_LOCKDOWN_LSM`/`IMA_APPRAISE_MODSIG` 强制启用 module signing，已在新 build dir 的 `.config` 中关闭（lockdown 运行时为 FORCE_NONE，IMA modsig 对 sim guest 无实际作用）。
   - 远端 kernel build dir：`~/ub_sim_kernel_build`（`REMOTE_CROSS_COMPILE=` 空，用 native gcc）。
   - 到 n4-910c 的 SSH 偶发断连，已在 `~/.ssh/config` 加 `ServerAliveInterval 15`。
5. **可复现命令**（本机）：

```bash
cd guest-linux/aarch64
ARTIFACT_SOURCE=remote ALLOW_REMOTE_LINUX_ARTIFACTS=1 \
REMOTE_LINUX_HOST=n4-910c \
REMOTE_KERNEL_SRC=/home/ll/ub_sim/guest-linux/kernel_ub \
REMOTE_KERNEL_BUILD=/home/ll/ub_sim_kernel_build \
REMOTE_TMPDIR=/home/ll/ub_sim_kernel_tmp \
REMOTE_LINQU_DRIVER_DIR=/home/ll/ub_sim/guest-linux/aarch64/driver \
REMOTE_CROSS_COMPILE= REMOTE_REUSE_KERNEL_CONFIG=1 \
BUILD_ON_REMOTE=1 BUILD_LINQU_DRIVER_ON_REMOTE=1 JOBS=64 \
AARCH64_LINUX_CC=aarch64-unknown-linux-gnu-gcc \
BUSYBOX=$PWD/busybox-aarch64 \
./scripts/build_guest_artifacts.sh
```

## 日志位置

- guest/qemu 日志：`guest-linux/aarch64/logs/2026-08-14_12-14-10_obmm_pool_16768/`
- 构建日志：`/tmp/guest_artifacts.log`、`/tmp/qemu_build.log`、`/tmp/obmm_pool_run.log`（host）
