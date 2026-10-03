# openEuler demo 补齐与远端验收

日期：2026-10-02。Console 的 19 个 openEuler demo 已补齐，模型矩阵、
Direct EL0、最终通用 demo 复验及完整远端回归均通过。

## 执行环境与版本

构建、单元测试、完整回归、浏览器测试、QEMU 和模型验证全部在 `n4-910c`
的独立 checkout 执行。本地 macOS 只编辑、同步源码和接收小型结果文件。
本轮没有启用网络操作审计，没有执行 git push。

Guest 使用 openEuler 24.03 LTS-SP2 qcow2 基础镜像和项目自建 AArch64 kernel。
普通 demo 通过 `init_switch_root` 部署当前应用，在 systemd 下执行原有
`run_app`；每个节点使用新的运行磁盘，基础镜像不嵌入本次 demo。

| 仓库 | 验收基线 |
| --- | --- |
| 主仓库 | `7e04d257002e77f59bd5a309f4a56fb3bb6ffb96` + 本次工作区修改 |
| QEMU | `f8e44ee732d1fad8d2270ea30877fc9bb4b0d503` + Busy 处理修复 |
| Kernel | `c2def7c931e6451d89727dec0c6f892abe28ba00` |
| Memory Service | `546985e26bce60798fa3f88329c3fbed8350c798` |
| libobmm | `53011eed10716b422d2ac29199f68b55f7c5bdc5` |

主仓库已 pull 上游更新，已初始化的 25 个子模块已更新到仓库 pin。
QEMU 可选固件 edk2/skiboot 的 pin 存在远端 `not our ref` 问题，未改写这些
gitlink；本轮 AArch64 direct-kernel demo 不依赖它们。

QEMU 通过项目 `build_qemu_binary.sh --with-obmm-tests` 重建，单独使用
`build-os-20261002` 目录。guest driver、应用和模块通过标准
`build_guest_artifacts.sh` 重建。验收时以上修改尚在工作区，子模块 gitlink
保持验收基线。

2026-10-03 按用户要求提交这组已验收的修改。QEMU 修复保存为
`f2ef54f454b661e551a8619f9feb7406752a1b1a`，主仓库 gitlink 记录该提交。
提交前在远端比对了 36 个文件，确认源码与上述验收版本一致；随后只更新了
文档中的提交状态和 QEMU gitlink。没有执行 git push；跨设备同步时应先同步
QEMU 提交，再同步引用它的主仓库提交。

## 补齐内容及影响

- Console 的 19 个 demo 均声明 openEuler，启动前检查注册目标的磁盘镜像。
  界面分别显示执行目标与 Guest OS；没有历史 OS 元数据的运行显示 unknown。
- 新增统一 CLI `run_ub_demo.sh`，保持参数边界，支持显式 openEuler 和
  initramfs 选择。普通 launcher 共用 boot 准备与清理，W5 沿用既有部署入口。
- 每次部署当前 initramfs 的应用和模块，使用新的节点磁盘。正常结束、Stop
  和失败只清理该次运行拥有的资源；公共 boot 准备函数也显式清理中途失败。
- 标准 native build 一次构建 netfs、fscache、9pnet、9pnet_virtio、9p 的
  模块依赖图。启用但缺失的模块会使构建失败，内置依赖不要求额外模块文件。
- 旧八节点 shell 的 Docker argv 正确传递内存、vCPU、kernel 参数和 guest
  目录。实测默认 8 GiB、4 vCPU、`pmd_mapping=100%`、`obmm.mempool_size=0`。

### Direct EL0 的实际缺陷

原目录中的默认 coroutine 参数执行 async-poll，不能用它证明 Direct EL0。
目录现在显式选择 scalar async-load，并要求正的 `direct_el0_upcalls` 和
`kernel_hotpath_ioctls=0`。

真实运行暴露了两个问题：恢复上下文时新事件进入 event ring，Busy 被转换成
协议错误并触发 SIGTRAP；scheduler-enter SVC 成功后没有把返回值写回 x0，
产生错误的正数返回值。修复跨 QEMU device、guest driver 和 EL0 runtime：

- QEMU 对尚未消费事件返回可重试 Busy；driver 将其转换为 `-EAGAIN`。
- runtime 先消费事件，保留 replay token 和上下文，再重试恢复；真正的协议
  错误仍失败。合并完成事件中先处理没有 interrupted PC 的旧 completion。
- 失败的 SVC 恢复保留 scheduler 的 ABI 状态，成功的 scheduler-enter 返回 0。

最终连续三轮、每轮两个节点均通过：

| 轮次 | 节点 A/B completed | 节点 A/B direct upcalls | 热路径 ioctl | A/B Busy retries |
| --- | --- | --- | --- | --- |
| 1 | 128 / 128 | 128 / 128 | 0 / 0 | 10 / 17 |
| 2 | 128 / 128 | 128 / 128 | 0 / 0 | 10 / 13 |
| 3 | 128 / 128 | 128 / 128 | 0 / 0 | 9 / 16 |

六个节点摘要的 failures、timeouts 和 replay mismatch 均为 0。
SVC 上下文恢复及 IRQ 唤醒仍经过 guest kernel，本次结果限定为上述热路径指标。

## Demo 与兼容路径验收

| 项目 | 节点规模 | 结果 |
| --- | --- | --- |
| URMA RPC | 2 / 4 / 8 | 通过；最终版本八节点复验通过 |
| OBMM Pool | 2 / 4 / 8 | 通过；最终版本八节点复验通过 |
| GSVA identity | 2 / 4 / 8 | 通过；最终版本八节点复验通过 |
| Memory Service | 2 | 最终版本复验通过 |
| Direct EL0 Upcall | 2 | 三轮通过，真实 async-load |
| Qwen3-0.6B managed serving | 2 / 3 / 4 / 8 | 每个规模 2 step，通过 |
| DeepSeek-V4-Flash managed serving | 2 / 3 / 4 / 8 | 每个规模 2 step，通过 |
| 交互式 openEuler RPC 与 Stop | 2 | OS、PID 1 systemd、串口与清理通过 |
| 显式 initramfs RPC | 2 | 兼容路径通过 |
| 旧 openEuler shell | 8 | up/status、9p、工具、内存 smoke 与 Stop 通过 |
| Console 浏览器 fixture | desktop / mobile | 通过 |

最初通用矩阵为 11 个用例，额外包含默认 async-poll 的兼容检查。
Direct EL0 的结论只采用上面的真实 async-load 三轮结果。
八节点 RPC 覆盖全部 56 个有向节点对和 112 次操作。

Qwen 的各规模输出 token IDs 均为 `[264, 3644]`；DeepSeek 均为 `[201, 41]`。
DeepSeek 使用既有 IQ2 GGUF：
`DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf`。

每个模型用例的每个节点均匹配 2 个 guest request 与对应 QEMU typed
reads/writes，确认正的读写计数、`shared_payload_staging_bytes=0`，以及请求
结束时对象、backing、地址、export/import mapping、引用、在途操作和隔离资源
九项计数归零。该结果限定于模型 range 的可观察边界，不代表完整链路零复制。

旧 shell 检查了八个新建的私有节点磁盘。该镜像的旧 QEMU 目录没有 qemu-img，
因此使用其已有完整复制兼容路径；普通 demo 和 managed serving 的 qcow2
backing overlay 路径另有真实运行及 qemu-img metadata 测试。

## 完整回归与命令

最后一次清理修复后，最终全套 guest 回归 752 个通过，耗时 551.678 秒；
Rust workspace 1,017 个通过、16 个既有 ignored，退出码为 0；格式检查通过。
QEMU 的 9 组单元测试共 57 个通过。

Rust 日志包含隔离子进程的重复 test result。这里仅统计
`filtered out=0` 的 45 个 workspace 主结果，重复运行未另计入通过数。

所有命令在远端仓库根目录运行，环境通过 ignored `.env` 文件加载。

```sh
set -a
source out/w6-pp-validation-20260930-tests.env
set +a
python3 -m unittest discover guest-linux/aarch64/tests
cargo fmt --all -- --check
cargo test --workspace --offline
```

运行验收加载 `out/demo-os-qemu-build-20261002.env`，通过正式 CLI 启动。
下面展示八节点 RPC 与 Direct EL0 的参数；磁盘路径替换为注册目标的镜像：

```sh
guest-linux/aarch64/scripts/run_ub_demo.sh \
  --guest-engine openEuler --open-euler-disk-image /path/to/rootfs.qcow2 \
  -- run_ub_eight_node_rpc_matrix.sh

guest-linux/aarch64/scripts/run_ub_demo.sh \
  --guest-engine openEuler --open-euler-disk-image /path/to/rootfs.qcow2 \
  -- run_ub_dual_node_apps.sh --app obmm_async_coroutine --iterations 3 \
  --async-load-model 'v3|enabled=1|contexts=64|pending=64|events=128|clock_mhz=2000' \
  --obmm-async-args '--mode async-load --coroutines 8 --inflight 1 --lookahead 0 --access-bytes 8 --pattern sequential --compute-us 0 --iterations 128 --warmup 0 --deadline-us 1000000 --seed 1 --verify'
```

模型调用 `run_w5_cluster_config.sh --managed-serving --model <weights>
--nodes <2|3|4|8> --steps 2 <config.env>`，经统一 CLI 传入镜像。
本轮生成的验收驱动和严格逐请求 AWK 校验保留在 ignored `out/`：

```sh
node out/demo-os-model-acceptance-20261002.cjs "$PWD"
node out/demo-os-final-runtime-20261002.cjs "$PWD"
node out/demo-os-final-evidence-20261002.cjs
```

## 证据与边界

小型结果位于 `out/oe-model-20261002-*.json`、对应 `.check.txt`、
`out/demo-os-acceptance-20261001.json`、`out/demo-os-interactive-20261002.json`
和 `out/demo-os-legacy-shell-20261002.json`。详细日志保留在远端的 `out/` 与
`guest-linux/aarch64/logs/`，没有搬运权重、磁盘和完整大日志到本地。

最终全套结果使用 `demo-os-guest-tests-r7-20261002` 和
`demo-os-cargo-tests-r7-20261002` 的 `.log/.exit`；最终通用运行结果为
`out/demo-os-final-runtime-20261002.json`。汇总程序核验退出码、全部模型
token、EL0 指标、运行资源清理和源码 diff，生成
`out/demo-os-final-evidence-20261002.json`。

最终汇总验收通过。全部模型运行的 overlay 和生成的 boot image 均已清理，
最终四个通用复验也没有留下它们拥有的 boot 目录或 QEMU 进程。
最终 `pgrep -fl '[q]emu-system-aarch64'` 检查没有运行中的 QEMU；本轮 shell 和
浏览器容器均已移除。主仓库及 QEMU 子模块的 `git diff --check` 通过，修改的
源码没有 DOS 换行。失败诊断所用的临时 boot tree/cpio 已清理，诊断日志保留。

本轮 managed serving 覆盖每个节点规模的两个 decode step；没有重新执行
2051-position 长历史接续，也没有扩大为完整 logits parity 或硬件 NPU 验收。
Rust 的既有 ignored 测试涉及外部官方 checkpoint、host Metal 或专用 native
fixture；没有把这些未执行项计入通过数。
