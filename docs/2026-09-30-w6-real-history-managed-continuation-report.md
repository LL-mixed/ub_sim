# W6 真实长历史 Managed Continuation 验收报告

日期：2026-09-30。

## 1. 结论和范围

**#1 的功能验收通过：全部 2051 个历史位置由真实模型逐步生成，完整状态
经格式转换、真实 Memory Service 发布和映射后，在 3-node W6 上继续执行
全部 43 层，触发 21 个 ratio-4 层的 513 行 PTO top-k 分支。**

本轮生成 token `35`，对应 tokenizer 文本 `A`。每个 range 的 managed
读写计数均为正，`shared_payload_staging_bytes=0`。三个节点的初始历史对象
均在 reader 释放后回收；最终对象、地址、映射、引用、在途操作和隔离资源
计数全部归零，未留下本轮 QEMU 进程。

验收边界明确如下：

- 前 2051 个位置由未修改的 DS4 CPU 真模型计算，保留完整逐步 token trace。
- 第 2052 个位置由 W6 的真实 PTO callable 5 执行，通过正常 PP handoff
  串联三个节点；该段执行没有调用 DS4 动态库。
- 历史包含所有层的 raw KV、compressed KV、indexer KV 和 compressor state。
  集成输入没有使用填零、重复行、伪造状态或降低 top-k 门限。
- 本轮覆盖 3-node、一个长历史接续 step；长历史 2/8-node 和连续 4/8-step
  尚无本轮运行证据。已有短序列矩阵的覆盖范围保留在 W6 计划中。
- DeepSeek 与 DS4 的完整 logits 精度对齐继续按用户决定暂缓。
  输出 token 成功、状态尺寸正确和资源清理成功不构成 logits parity 验收。
- 本轮验证没有修改任何子模块源码或 gitlink。随后已依次发布 Kernel、QEMU、
  Memory Service 和基础检查点 `daaebf00`，并核实 GitHub 分支 SHA。
  本报告对应的接续代码保存为后续主仓库提交，其 GitHub 同步仍待执行。

## 2. 真实历史来源

真实输入在 `n4-910c` 上生成，使用已有 IQ2 GGUF 权重：

```text
DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf
```

生成器为 `guest-linux/aarch64/tests/deepseek_v4_flash_history_reference.c`。
它链接未修改的 DS4 public CPU API，每个位置调用 `ds4_session_eval`，读取
完整 logits，通过 argmax 选择下一 token，最后调用 public session payload
保存接口。初始 raw token 为 `42`；后续每个输入均取上一位置的真实 argmax。
本轮没有添加文本 prompt 或 chat template。

| 项目 | 已记录值 |
| --- | --- |
| 历史位置数 | 2051 |
| 每个位置执行层数 | 43 |
| Session context | 2052 |
| CPU threads | 12 |
| 历史生成耗时 | 1030.414 s |
| Native checkpoint | 52,184,472 bytes |
| 下一输入 token | 201 |
| 每层 live raw rows | 128 |
| 21 个 ratio-4 层 | 每层 512 compressed rows 和 512 indexer rows |
| 20 个 ratio-128 层 | 每层 16 compressed rows |

生成阶段校验了 2051 条 trace 与 payload token 序列的一致性、相邻 token 链、
最终 logits argmax、全部层状态几何和精确文件边界。

| 对象 | SHA-256 |
| --- | --- |
| DS4 source | `6be99cdae47b9bdf241cde9619f5674545a21e10e32540f03c68442fae7912cd` |
| Native payload | `06fe1e0365b8b0447ad07dc3a66ae1a6fa95cb568e23eae7b6984804825b870a` |
| Autoregressive trace | `03c0a33b22b9ab4814b7808d175b7de3f6a670081ef7c5dc9b53f817cb07e84e` |

原始生成证据位于目标机示例路径
`n4-910c:/home/ll/sim-console/ub_sim/out/w6-range-parity/`，包括
`real-history-2051.payload`、`.jsonl`、`.log`、生成验收 JSON 和 source/build
receipt。该目录的 DS4 CPU object 使用 GCC 10.3.1。生成器及全部链接 object
的摘要已保留。完整历史和权重均未搬到本地开发机器。

## 3. 转换和服务路径

### 3.1 无损格式转换

新增测试 CLI：

```bash
python3 guest-linux/aarch64/scripts/prepare_deepseek_history_checkpoint.py \
  convert --payload out/real-history-2051.payload \
  --trace out/real-history-2051.jsonl \
  --output out/w6-history-2051-3node --nodes 3

python3 guest-linux/aarch64/scripts/prepare_deepseek_history_checkpoint.py \
  validate --directory out/w6-history-2051-3node --nodes 3
```

转换器完整解析 native version 2 数据与 trace，逐层复制原始 FP32 字节，
只添加 W6 range 元数据并调整序列化布局。它没有重算、量化或替换 KV 内容。
43 层按正常 PP 规则分为 `[0,15)`、`[15,29)` 和 `[29,43)`；相同转换器
支持 2/3/4/8-node 的格式切分。

每个节点文件包含 64-byte versioned envelope：magic、version、past、next
token、layer bounds、state length 和 checksum。文件 body 使用现有 W6
range-state 编码。`receipt.json` 保留输入摘要、每层状态摘要和输出文件摘要。
校验 CLI 要求精确的几何、分区、长度、checksum 和 SHA-256，拒绝损坏或截断。
转换输出目录必须不存在，已有 fixture 不会被覆盖。

### 3.2 真实 Memory Service 与 PTO 执行

新增单一测试配置项 `SIM_W6_TEST_DEEPSEEK_HISTORY_DIR`，默认不设置。
入口只接受 DeepSeek managed-direct 单请求，拒绝 queue、多请求文件、错误
模型和缺失目录。目录通过正常 initramfs 或 openEuler root overlay 放到 guest
的 `/opt/ub_sim/deepseek_history`，各节点取得自己的状态文件。

`llm_infer.c` 的 `w6_managed_import_history()` 执行以下顺序：

1. 校验 envelope 与当前节点的 layer bounds、输入 token 和状态尺寸。
2. 通过正常 SDK 打开 managed writer，并获取 CPU write lease。
3. 将文件 body 一次性写入该 managed allocation，校验 checksum 与 W6 header。
4. 发布 payload range，stage KV reference，释放 CPU lease，然后 seal writer。
5. 关闭 writer，使用正常 SDK reader 取得该 sealed KV reference。
6. callable 5 消费这一 mapped previous-KV view，通过 managed `UB_GM`
   和 PTO `TLOAD/TSTORE` 完成模型 range；其 next KV、hidden 和 terminal logits
   保留在正常 managed output backing。
7. 原 CQ 完成后释放 reader，再回收初始 allocation；请求结束执行正常资源清理。

初始历史使用当前 run/request/producer 的 allocation key，step 使用
`UINT64_MAX` 哨兵值，独立于正常 decode step。它沿用已有 service allocation、
generation、holder 和 lease 机制，没有另设 object store 或绕过服务映射。
错误或未完成释放保留拥有的句柄供后续清理。

**复制范围：**文件导入有一次性写入成本，本轮共导入 51,662,016 bytes 的 W6
range-state。`shared_payload_staging_bytes=0` 针对模型 range 的执行边界，
证明执行时共享 KV/hidden/logits 没有被搬到 Rust `Vec`、host tensor 或 inline
UAPI payload。它不能用于宣称 checkpoint 文件到 OBMM 的整段导入零复制。

## 4. 实际运行配置

接续目标为 `n4-910c1`，在独立 checkout 的 openEuler QEMU guest 中运行。
目录示例：`/home/ll/w6-clean-20260930/ub_sim`。配置放在 ignored `out/`。

```bash
# out/w6-real-history-continuation.env
source ./out/w6-clean-deepseek.env
SIM_LLM_INFER_PROMPT_TOKEN_IDS=201
SIM_QWEN3_GUEST_DECODE_STEPS=1
SIM_W6_TEST_DEEPSEEK_HISTORY_DIR=/home/ll/w6-clean-20260930/ub_sim/out/w6-history-2051-3node
SIM_W5_MANAGED_TIMEOUT_MS=900000
SIM_W5_MANAGED_PROVIDER_LEASE_MS=900000
SIM_W5_PTO_UB_GM_TIMEOUT_MS=900000
APP_WAIT_SECS=1800
SIM_W5_PROGRESS_INTERVAL_SECS=60
```

复用的 `w6-clean-deepseek.env` 选择 DeepSeek IQ2 GGUF、正常 managed service、
openEuler engine 和已有 base image；QEMU 每节点配置为 8 GiB、2 vCPU。
执行入口保持原有项目命令：

```bash
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh \
  --nodes 3 --steps 1 out/w6-real-history-continuation.env
```

Run ID：`2026-09-30_16-56-40_w5_deepseek_v4_flash_decode_11420`。
入口名称和部分日志仍包含 W5，实际模型 range 的 callable identity 全部为 `5`。
QEMU wrapper 发现 sim-qemu staticlib 更新后自动 relink，运行使用本轮 Rust 修复。

## 5. 运行证据

### 5.1 完整模型执行与 top-k

| 节点 | 层范围 | Previous KV bytes | Next KV bytes | Ratio-4 rows | Top-k calls | PTO argmax calls |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| A | `[0,15)` | 17,024,048 | 17,041,968 | 513 | 7 | 7,168 |
| B | `[15,29)` | 17,318,984 | 17,336,904 | 513 | 7 | 7,168 |
| C | `[29,43)` | 17,318,984 | 17,336,904 | 513 | 7 | 7,168 |

三个节点覆盖全部 43 层；21 个 ratio-4 层实际进入 score-and-select 分支，
总计 21 个 top-k 和 21,504 个 PTO argmax 调用。新增状态为 53,760 bytes，
等于 `21 * (512 + 128) * sizeof(float)`，与新增 attention/indexer 行一致。

节点 A 的实际 kernel 日志：

```text
deepseek-pto-history: layers=0:15 past=2051 tokens=1 ratio4_rows=513 topk_calls=7 argmax_calls=7168 previous_bytes=17024048 next_bytes=17041968 status=pass
```

### 5.2 Managed 读写、GSVA 与发布

| 节点 | Callable | Managed read calls | Managed write calls | Shared staging bytes | GSVA bind events |
| --- | ---: | ---: | ---: | ---: | ---: |
| A | 5 | 356,152 | 13,280 | 0 | 6 |
| B | 5 | 356,064 | 13,301 | 0 | 6 |
| C | 5 | 356,064 | 142,581 | 0 | 6 |
| 合计 | 5 | 1,068,280 | 169,162 | 0 | 18 |

读写数为 dispatch-bound callback 调用次数；该表没有将调用次数解释为 bytes
或物理设备指令数。GSVA 列来自实际 `QEMU_UB_GM_GSVA_BIND` 标记。

```text
[w6_history] node=1 layers=0:15 past=2051 token=201 bytes=17024048 checksum=0x760d2d7f82273d24 allocation=w5v1/2e289a1b54a5e458/6dd944938c609604/00000000/ffffffffffffffff generation=1 published=sealed reader=managed status=ok
pto-ub-gm-model-access: request=12037984203823726571 callable=5 reads=356152 writes=13280 shared_payload_staging_bytes=0 status=pass
[w4_guest] stage w5_managed_model_range_complete node=1 step=0 layers=[0,15) next=2 callable=5 model_io=direct inputs=1 outputs=2 hidden_bytes=65536 kv_bytes=17041968 hidden_checksum=0xedac284d3790e5c5 kv_checksum=0x86052d9c36bbad82 allocation_home=guest-node-0 terminal=0 status=ok
```

末节点实际发布记录包含 `token=35`、`source=managed_callable5_logits`、
`target_kind=mem_service_reference_v2` 和 `notification=reference_committed`。
集群入口最终输出：

```text
decode: steps=1/1 passed_nodes=3/3 suffix_replay_steps=0 effective_generated_steps=1
decode_output: token_ids=[35]
decode_output: token_pieces="A"
PASS: W5 inference cluster nodes=3 profile=deepseek_v4_flash_decode
3-node W5 inference cluster validation passed
```

入口记录的 round 时间为 294,620 ms。该数包括带长历史状态的本轮执行，
未建立性能比较基线，不用于性能收益结论。旧 prefix/compatibility 汇总中的
`gsva kv_refs=0` 和 `actual_range_forwards=0` 不统计本轮 callable-5 managed
模型路径；本轮验收使用上述实际 kernel、host access、GSVA bind 和 SDK 标记。

### 5.3 生命周期和进程清理

初始对象 allocation generation 分别为 `1`、`3`、`5`。三个节点均先关闭
reader，再退休相同 generation 的对象：

```text
[w6_history] node=1 generation=1 state=retired reader_released=1 status=retired
[w4_guest] stage w5_managed_request_cleanup node=1 steps=1 allocations=3 final_generation=6 live_objects=0 backing_bytes=0 address_bytes=0 export_mappings=0 import_mappings=0 live_refs=0 in_flight=0 quarantined_objects=0 quarantined_bytes=0 status=ok
```

严格证据检查对每个节点要求九个清理字段全部为零、状态为 `ok`，并检查
`/proc` 中带本轮 run ID 的 QEMU cmdline。结果为 `status=pass`、
`run_owned_qemu=[]`。其他运行已有的 QEMU 不在本轮清理范围。

## 6. 本轮发现并修复的问题

| 失败 run ID | 实际失败 | 根因与修复 |
| --- | --- | --- |
| `2026-09-30_16-29-34_w5_deepseek_v4_flash_decode_3496` | guest worker `rc=139`，未进入模型执行 | `main` 栈帧为 8,955,904 bytes，超过 guest 8 MiB stack limit；将已有 process-owned `db_service` 移至静态存储，保持原进程生命周期。修复后栈帧为 6,775,152 bytes，新增编译式 `<7 MiB` budget test。 |
| `2026-09-30_16-45-24_w5_deepseek_v4_flash_decode_14009` | 初始历史已封存和取得，CQ `status=3`，`deepseek_v4_flash_pto_range_memref_contract` | Rust state-size validator 仍对 compressed rows 应用 `min(top_k)`；C/C++ 已保留实际行数。Rust 改为保留全部行，只由 indexer 控制每查询选取 512 行，新增边界尺寸和错误小输出拒绝测试。 |

核心尺寸修复位于 `crates/sim-uapi/src/deepseek_v4_flash_pto_range.rs`：

```rust
let compressed_rows = u64::from(positions) / u64::from(ratio);
```

测试检查 `[0,15)` 在 2051/2052 位置分别需要 17,024,048/17,041,968 bytes，
并覆盖 ratio-4/ratio-128 超过 top-k 的行数增长以及拒绝截断输出。
栈修复没有提高 guest stack limit，也没有禁用任何失败检查。

一次中间全回归还发现 `--print-env` golden 未列出新测试字段；更新了精确期望
输出，断言保持启用，随后完整重跑通过。失败尝试均保留原日志，最终成功 run
独立记录，没有将失败日志替换为成功结果。

## 7. 最终回归和源码身份

所有模型、QEMU、编译式集成测试和完整回归均在远端运行，本地仅做轻量静态检查。

| 验证 | 结果 | 远端日志 |
| --- | --- | --- |
| `python3 -m unittest discover guest-linux/aarch64/tests` | 725/725，484.779 s，`OK` | `out/w6-history-final-full-guest-tests.log` |
| `cargo test --workspace --offline` | exit 0 | `out/w6-history-final-full-cargo-tests.log` |
| History converter、header、compiled stack budget | 9/9 | 远端 focused test 输出 |
| DeepSeek callable-5 ABI tests | 5/5 | `out/w6-history-abi-tests.log` |
| Long-history 3-node integration | 3/3 nodes，1/1 step，exit 0 | `out/w6-real-history-final-continuation.log` |
| 严格证据和资源复核 | `status=pass` | `out/w6-real-history-acceptance.json` |

Rust workspace 保留原有 `ignored` 测试标记，本轮没有取消或新增这些标记。
exit 0 对应完整默认启用测试集合，不能用于宣称 `ignored` 用例也已执行。
本地 `cargo fmt --all -- --check` 和 `git diff --check` 均通过。

上述验证运行时，远端独立 checkout 的 root HEAD 为
`32a0c7d97a5cf76620fc721ff46cd4f7daf4c0dd`，叠加历史生成器和本轮
continuation 工作区源码；当时本地基础 HEAD 为 `daaebf00`。本报告与接续
代码作为该基础检查点之后的主仓库提交保存。实际运行源码通过下表 SHA-256
记录，不能仅凭当时的 root HEAD 描述测试身份。关键文件本地摘要与远端
运行 receipt 完全一致。

| 源文件 | SHA-256 |
| --- | --- |
| `guest-linux/aarch64/apps/llm_infer/llm_infer.c` | `39c81fb83cd46d96d5304808e9e3f76fd838be94814502d71e628401cd1d5b7a` |
| `crates/sim-uapi/src/deepseek_v4_flash_pto_range.rs` | `c079c5fd72bda9d5947c21dfbb3f2057864df1729a5fe92d4d5d5f5e1b353f7b` |
| `guest-linux/aarch64/libs/lingqu_shmem_pto/deepseek_v4_flash_pto_range_kernel.cpp` | `0240bd14c51bfefcbeb0a29e950259d9ede63443f7757c3243d669bd579107f3` |

本轮未改变的关键子模块：

| 子模块 | Revision |
| --- | --- |
| `mem_service` | `546985e26bce60798fa3f88329c3fbed8350c798` |
| `guest-linux/kernel_ub` | `c2def7c931e6451d89727dec0c6f892abe28ba00` |
| `vendor/qemu_8.2.0_ub` | `f8e44ee732d1fad8d2270ea30877fc9bb4b0d503` |
| `vendor/simpler` | `3794c4309fbdb8d20a262ced4e7cb1b3fa823261` |
| `vendor/pto-isa` | `6a9da1e1e9571f98de1c43d88d58478804948410` |

## 8. 证据位置和后续边界

远端完整 raw logs 位于独立 checkout 的
`guest-linux/aarch64/logs/<run-id>_headless8/`，包含 A/B/C guest 和 QEMU log。
`out/w6-real-history-acceptance.json` 包含逐节点 excerpt、所有验收字段及完整
raw log SHA-256；`out/w6-history-2051-3node/receipt.json` 包含转换来源和输出
摘要。证据 checker 为 ignored 的任务专用工具 `out/w6_check_real_history.py`。

仅将 compact acceptance、receipt、source hashes 和 launcher log 取回本地
`out/`。checkpoint、model weights、raw 大日志和网络审计全文均不提交。
SSH/SCP 操作按项目网络审计规则登记；该登记覆盖可观测工具操作，不能作为
整机抓包证据。

功能 gate #1 和先前 clean-checkout gate #2 均已完成。后续明确区分：

- 子模块与基础检查点已发布，gitlink 和 Memory Service lock 均已核实。
  后续接续提交的 GitHub 同步、GitHub-only clean clone 及其 fresh-build
  复现仍待执行。
- 更广并发、产物 retention，以及其他长历史拓扑/step 仍属于新增覆盖。
- DS4 完整 logits 对齐继续暂缓；1M context 不在验证范围。
- 本轮测试用 checkpoint adapter 不承诺 production checkpoint 兼容 API。

当前计划见 [W6 managed model-direct plan](plans/2026-09-22-w6-managed-model-direct-plan.md)，
测试配置说明见 [W5 test environment inventory](w5_test_env_inventory.md)。
