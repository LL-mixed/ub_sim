# W5 terminal token 可见性调查

日期：2026-09-07。执行目标：`n4-910c1`。

## 调查结论与范围

已确认一条真实的连续 decode 故障路径：配置
`SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE=1` 后，Qwen3 的前置 token
等待会读取未刷新的远端导入映射。4-node 请求完成两个 token 后，
nodeA 在进入 decode step 2 时等待 token 1，流水线停止推进。
原样运行最终出现 600 秒 token 等待超时并以 exit 1 结束。
仅补齐 terminal reader 的远端刷新后，相同故障配置在 4-node 和
8-node 均完成全部 4 steps，token 序列与成功基线一致。

同一版本、相同机器、相同模型，未设置该开关的 4-node 与 8-node
4-step 均已通过。4-node 单独添加 `--gsva-kv` 也通过。因此故障范围
需要包含请求配置；不能由截图推导所有 W5 配置都失败。

本报告针对 GitHub 根仓库的精确子模块组合。DeepSeek、真实 prefix 命中、
serving queue、多请求混跑均未纳入本次运行验证。

## 版本与环境

| 项目 | 值 |
| --- | --- |
| 根仓库 GitHub master | `0fecb3b1037a424d7d285ed8c2792f08031e88ed` |
| mem_service | `4b13ec0eae2d1f91a1e203fe2644c9df1e0765f6` |
| QEMU | `15951308ea8fa1fbce600d434a5b8cf72e132f14` |
| kernel_ub | `70c7272a8b66bc9981a2fa3235ae71489ceb7789` |
| simpler | `8a6a28f405c8e46a91c15216478dd7a4acc29aed` |
| 隔离检出 | `/home/ll/w5-github-regression` |
| Docker 容器 | `w5-github-regression` |
| 容器镜像 | `openeuler-2403:v0.0.5` |
| Guest | initramfs Linux，4 或 8 个 QEMU 实例 |
| 模型 | `/home/ll/models/Qwen3-0.6B` |
| 默认 prompt token IDs | `81378,37585,374` |
| Decode | 4 steps，greedy top-k=1 |
| 每节点配置 | `QEMU_MEM=8G`、`QEMU_SMP=2` |

根仓库由 GitHub fetch 后创建独立 worktree；子模块从目标机已有 Git
对象缓存建立独立检出，并 checkout 根仓库记录的精确 SHA。
Rust、guest 用户程序和 QEMU 均为此次检出重新编译。
kernel Image 复用目标机已有的同 SHA、同 build_policy=3 构建产物；
其 SHA-256 为
`c12157f80a5ad56738563623b91ff004ae8e8ca10b16bd0b416f4f91aa92a18d`。

QEMU 重建产物 SHA-256：
`c756782b1c65ba50f2c5ceafabe0447da73c0ffea673675ccdf7a16c5d43a4ca`。
后续 reader 对照共用该二进制。

## 运行矩阵

| 配置 | 节点 | Forwards | Terminal tokens | 结果 |
| --- | ---: | ---: | ---: | --- |
| w5.env | 4 | 16/16 | 4/4 | PASS |
| w5.env + --gsva-kv | 4 | 16/16 | 4/4 | PASS |
| w5.env + --gsva-kv | 8 | 32/32 | 4/4 | PASS |
| 上述配置 + SHORTPATH_EXECUTE=1 | 4 | 8/16 | 2/4 | 超时，exit 1 |
| 定点刷新 reader + SHORTPATH_EXECUTE=1 | 4 | 16/16 | 4/4 | PASS，exit 0 |
| 定点刷新 reader + SHORTPATH_EXECUTE=1 | 8 | 32/32 | 4/4 | PASS，exit 0 |

五组成功运行均输出 `[264, 3644, 7653, 304]`，文本为
` a global leader in`。`--gsva-kv` 表示该项命令行配置已打开；
本次未验证 prefix cache 正向命中或其性能收益。

故障运行 ID：
`2026-09-07_07-55-24_w5_qwen3_0_6b_decode_20849`。
nodeD 日志确认：

```text
model_terminal_token_result_publish local=node4 target=node1
step=1 token=3644 epoch=2 seq=4 status=ok notification=delivered
```

nodeA 已读取 token 0，并完成两次 forward；进入 step 2 后，最后一条
业务日志停在 `llm_infer_prompt_tokens_seeded`。
nodeB、nodeC、nodeD 均停在 step 2 的
`model_work_item_scheduler_wait`。

4-node 修复对照 run ID 为
`2026-09-07_08-07-01_w5_qwen3_0_6b_decode_6570`；
8-node 修复对照为
`2026-09-07_08-08-31_w5_qwen3_0_6b_decode_8669`。

原故障的最终日志：

```text
[mem_service] gap model_terminal_token_result_wait=timeout step=1
[w4_guest] fail previous terminal token missing model=qwen3-0-6b step=1
guest_worker_summary: pass=false steps=4 range_forwards=8
runtime_inputs=7 runtime_outputs=8 terminal_tokens=2
```

刷新版本的 4-node 日志记录了前置 reader 成功获取 step 0、1、2 的
token，包含原来缺失的 step 1/token 3644。4-node 最终结果为
`pass=true range_forwards=16 terminal_tokens=4`；8-node 为
`pass=true range_forwards=32 terminal_tokens=4`。

## 根因

调用关系位于 `guest-linux/aarch64/apps/llm_infer/llm_infer.c`：

1. `SHORTPATH_EXECUTE=1` 且本地 token history 落后时，先调用
   `mem_service_obmm_service_v0_wait_terminal_token_result`。
2. 调用最终进入
   `mem_service_model_terminal_token_flow.c` 的
   `mem_service_wait_terminal_token_result_for_model`。
3. 该函数检查远端映射，读取 compact summary、对象记录和 64 字节
   payload，校验 step 与 checksum。
4. 此路径没有刷新远端映射，也没有消费 token descriptor。

`mem_service_activate_remote_slot` 对已有映射直接返回成功。
`mem_service_try_read_stable_compact_summary_region` 和
`mem_service_slot_find_record` 使用映射中的字节；volatile 读取与本地
内存屏障不执行远端同步。

在当前 OBMM 模拟实现中，这条路径需要调用已有的
`mem_service_sync_remote_range`。底层通过
`OBMM_SHMDEV_SYNC_REMOTE_RANGE` 更新导入映射中指定范围的内容。

这解释了固定的轮次边界：第一次导入时 token 0 可见，后续映射复用时
token 1 元数据和 payload 没有得到刷新。nodeA 阻塞在前置等待，
因此尚未到达正常调度器的 descriptor 消费路径。默认配置通过的日志中，
token 0、1、2 的 descriptor 均正常消费，`attempts=1`。

`notification=delivered` 记录的是发布侧通知成功，不能证明接收端已经
执行消费，更不能保证另一个对象扫描函数已取得最新数据。

## 修复对照

实验仅调整 terminal token reader：刷新稳定 header 和有效 records，
重新确认发布序列，再刷新对应的 64 字节 payload。保留原有边界、
step、类型与 checksum 检查。

实验提交仅存在于目标机隔离检出的
`mem_service/codex/w5-token-visibility-probe`：
`be8fdb08c9298c7a79e60ed3eacac98f62137782`。
实验根仓库的 lock 与 index gitlink 一起指向该 SHA。
该实验提交没有发布。后续正式修复基于 master 的 `5878d6e` 实现，
提交为 `e4a8a58fdb9f62c1ace4948eac5124ad701b8e67`，已发布到
`LL-mixed/mem_service/master`，目标机也已从 GitHub fetch 该提交。
根仓库的 gitlink 和 `mem_service.lock` 配对指向正式修复。

正式代码将原 range reader 的 metadata/payload 刷新函数移到共享的
`mem_service_cluster_read.c`，terminal reader 调用同一实现，同时补齐
同步范围和地址加法的边界检查。行为测试位于 mem_service 的正式
`tests/` 目录，编译实际 reader，通过独立的导出区和导入映射覆盖
滞后映射、本地读取、两类同步失败、发布序列不一致、checksum 错误、
对象边界、元数据边界和地址溢出，共 10 个场景。

该行为测试在旧 master 上的 stale-import 场景失败，修复后全部通过。
首次完整服务测试发现一项源码结构断言仍要求刷新代码位于旧文件，
已更新为检查共享模块实现以及 range/terminal 两条调用路径。

函数级测试编译并执行真实 token 等待函数、真实 record reader。
测试中的远端发布区与导入映射分开，模拟同步接口只在显式调用时复制
请求范围。它提供函数级证据，集群行为由上面的 QEMU 矩阵独立验证。

```text
baseline: stale_import rc=-1 token=11 sync_calls=0
refresh:  stale_import rc=0  token=22 sync_calls=4
checksum_mismatch=reject refresh_failure=reject torn_publication=reject
```

实验检出执行 `python3 -m unittest discover guest-linux/aarch64/tests`：
304 tests，OK。首次执行有一项 index gitlink 与实验 lock 未配对的失败，
配对后完整重跑通过。

## 正式提交复测

正式修复 `e4a8a58` 基于当前 mem_service master，包含已经发布的
`5878d6e`。该前置提交的 publish 日志增加了 `payload_mode` 字段。
第一次复测仍搭配 `0fecb3b` 的旧 runner，实际完成 16 forwards 和
4 tokens，但旧日志正则未识别该字段，最终 exit 1；日志保留在
`terminal-refresh-final4.log`，该次运行不计为 PASS。

随后将当前 ub_sim 已提交的 `run_llm_infer_eight_node_guest.sh`
同步到隔离验证目录，该脚本最近一次修改提交为 `83dc22b`。
没有删除断言或放宽检查。本次未将根仓库其他未涉及的源码和子模块
整体升级，复测组合为 `0fecb3b` 基础工作区、上述当前 runner、
正式 mem_service 提交及配对的 lock/gitlink。

| 正式修复配置 | Forwards | Tokens | 结果 |
| --- | ---: | ---: | --- |
| 4 nodes，4 steps，GSVA KV + shortpath | 16/16 | 4/4 | PASS，exit 0 |
| 8 nodes，4 steps，GSVA KV + shortpath | 32/32 | 4/4 | PASS，exit 0 |

两次 token 序列均为 `[264, 3644, 7653, 304]`。
运行 ID 分别为 `2026-09-07_09-22-55_w5_qwen3_0_6b_decode_25029`、
`2026-09-07_09-25-34_w5_qwen3_0_6b_decode_12563`。
日志为 `terminal-refresh-current-runner4.log` 和
`terminal-refresh-current-runner8.log`。

同步 runner 后的 guest 完整测试为 304 项，全部通过，日志为
`terminal-refresh-current-runner-guest-tests.log`。
正式 mem_service 提交完整复测为 136 项，131 项通过、5 项按条件跳过，
耗时 1044.241 秒，退出码 0；日志为
`terminal-refresh-service-tests-final.log`。跳过项包括两项要求
`aarch64-linux-gnu-gcc` 命名工具链的检查，以及三项要求 ub_sim 文件
直接位于独立服务仓库中的检查。目标机 guest 实际构建使用原生 GCC，
上述 W5 与下游测试单独验证了真实集成路径。
本地仅执行轻量的 mem_service 源码结构检查（44 项，3 项按条件跳过）
和下游锁定检查（10 项全部通过）；QEMU 和完整测试均在目标机容器中。

## 原截图的归因

截图的停滞现象已经在明确配置下复现。截图列出的三个 mem_service
优化提交主要调整 range reader 的 recovery 策略。当前发现的直接缺口
位于另一个 terminal reader；该缺口在更早的源文件中已经存在。
本次没有完成跨仓库成对版本 bisect，无法指定哪个历史提交首次使故障
稳定暴露。

Web demo 的 steps 默认值为 2。这样的默认运行无法覆盖进入 step 2
后的故障；已有源码结构检查也未覆盖远端导入映射滞后的行为。
正式回归应至少包括 4 steps、该开关开/关、4-node/8-node。
正式修复已让 range reader 和 terminal reader 共用稳定元数据刷新实现。

## 证据和复现入口

远端日志位于 `/home/ll/w5-github-regression/`：

- `baseline4.log`、`baseline4-gsva.log`、`baseline8-gsva.log`。
- `shortpath4.log`、`shortpath4-stalled-summary.json`。
- `refresh4.log`、`refresh8.log`：保持故障配置的修复对照。
- `visibility-test.log`、`guest-tests.log`、`refresh-build.log`。
- Guest 原始日志保留在该检出的 `guest-linux/aarch64/logs/*_headless8/`。

本地调查配置、测试程序与实验 reader 保存在
`out/w5-github-regression/`。目标机的基础 `w5.env` 与 `shortpath.env`
分别对应开关未设置和显式设置为 1。
主要运行日志已复制到该目录的 `evidence/`，各节点原始日志位于
`guest-logs/`，单文件实验补丁为 `terminal-token-refresh.patch`。

在目标机容器内、检出根目录运行：

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --nodes 4 --steps 4 --gsva-kv w5.env
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --nodes 8 --steps 4 --gsva-kv w5.env
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --nodes 4 --steps 4 --gsva-kv shortpath.env
```

复现原缺陷须使用 `4b13ec0` 的 mem_service 和对应 lock/gitlink；
实验修复检出使用 `be8fdb0`。不要把这两组源码与构建产物混用。
