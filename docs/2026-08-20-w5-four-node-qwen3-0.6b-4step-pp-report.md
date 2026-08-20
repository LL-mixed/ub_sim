# W5 四节点 Qwen3-0.6B 4-step 流水线并行推理验证报告

- 日期：2026-08-20
- 运行主机：本机（/sd_data 服务器，62G RAM / 32 核，QEMU 4 节点 × 8G）
- 结论：**PASS**——`run_w5_cluster_qwen3_0_6b_2step.sh` 以
  `SIM_W5_CLUSTER_NODE_COUNT=4 SIM_QWEN3_GUEST_DECODE_STEPS=4` 端到端退出码 0

## 1. 目标与结果

在本机以 4 节点拓扑运行 W5 qwen3-0.6B 解码集群，4 个 decode step，
流水线并行（PP）：28 层均分 4 段（4×7），环形转发，nodeD 出 token。

| 指标 | 值 |
|---|---|
| 退出码 | 0 |
| guest 判定 | `[w4guest8] PASS: W5 inference cluster nodes=4 profile=qwen3_0_6b_decode` |
| worker 计数 | range_forwards=16/16，runtime_inputs=15/15，runtime_outputs=16/16（4 节点 × 4 step） |
| 通过节点 | passed_nodes=4/4，worker_timing_records=16 |
| 生成 token | `[264, 3644, 7653, 304]` → `" a global leader in"` |
| 8 节点对照 | 同 prompt 同权重生成**逐字相同**文本，token 序列一致 |

## 2. 运行命令

```bash
cd guest-linux/aarch64
SIM_QWEN3_DENSE_WEIGHTS_PATH=/sd_data/lllm_serving/models/Qwen3-0.6B \
SIM_QWEN3_GUEST_DECODE_STEPS=4 \
SIM_W5_CLUSTER_NODE_COUNT=4 \
./scripts/run_w5_cluster_qwen3_0_6b_2step.sh
```

## 3. 流水线几何（guest 实测日志）

```
nodeA: stage uapi_qwen3_range_compute_contract node=0 layers=[0,7)  count=7 next=1 pipeline_nodes=4 total_layers=28
nodeB: stage uapi_qwen3_range_compute_contract node=1 layers=[7,14) count=7 next=2 pipeline_nodes=4 total_layers=28
nodeC: stage uapi_qwen3_range_compute_contract node=2 layers=[14,21) count=7 next=3 pipeline_nodes=4 total_layers=28
nodeD: stage uapi_qwen3_range_compute_contract node=3 layers=[21,28) count=7 next=0 pipeline_nodes=4 total_layers=28
```

终末发布链（nodeD → nodeA）：`qwen3_terminal_token_result_publish local=node4 target=node1 step=N token=... margin_milli=...`，
每 step 一条，4/4。

逐步 token 与 margin（summary 文件）：

| step | token | 文本片 | runner_up | margin_milli |
|---|---|---|---|---|
| 0 | 264 | " a" | 279 | 1205 |
| 1 | 3644 | " global" | 2813 | 32 |
| 2 | 7653 | " leader" | 2813 | 3468 |
| 3 | 304 | " in" | 323 | 4708 |

## 4. 时序（summary 文件）

| step | round_ms | 关键路径节点 | compute 窗口 ms | barrier ms |
|---|---|---|---|---|
| 0 | 16213（冷启动） | nodeC | 4572 | 10061 |
| 1 | 6312 | nodeC | 2892 | 5241 |
| 2 | 6166 | nodeB | 2734 | 5036 |
| 3 | 6520 | nodeD | 2940 | 0 |

稳态单轮 ~6.3s（其中 barrier 等待 ~5.2s 占主导——4 节点环形流水的
吞吐瓶颈在跨节点 input_wait/barrier，而非本地 compute）。

Memory Service 侧：boundary observations 12 条（4 step × 3 非首节点）、
decisions 12 条、continue=12，`status=ok`。

## 5. 为什么需要改代码：4 节点 PP 此前不存在

W5 qwen3 流水线此前硬编码为 8 段。本次提交 `2dc8f02`
（"Support four-node W5 pipeline-parallel inference"）解开以下硬编码：

| 层 | 文件 | 改动 |
|---|---|---|
| 集群接线 | `launch_ub_eight_node_headless.sh` / `run_llm_infer_eight_node_guest.sh` / `run_w5_cluster_config.sh` | 增加 4 节点拓扑臂（`ub_topology_four_node_full_mesh.ini` + `mvp_4host_single_domain.yaml` + nodeA-D + PORT_NUM=3），白名单 2/3/4/8 |
| guest 门 | `apps/llm_infer/llm_infer.c` | 新增 `w4_qwen3_tp_nodes()`（读 `SIM_QWEN3_DENSE_TP_NODES`，默认 8），7 处 `cluster_node_count == 8U` 改为 `== w4_qwen3_tp_nodes()` |
| host env | `run_w5_inference_cluster_runtime.sh` | qwen3 profile 默认 `SIM_QWEN3_DENSE_TP_NODES=$SIM_W5_CLUSTER_NODE_COUNT`（host 发布的 layer range 与集群规模对齐） |
| Rust profile | `crates/sim-cli/src/main.rs` `qwen3_guest_dense_runtime()` | tp_nodes 改为 env 可覆盖（原先固定 `QWEN3_DENSE_DEFAULT_TP_NODES`=8 并在 spawn 时覆盖外部 env） |
| Rust 判定 | 同上 | 期望计数 `8 * steps` 参数化为 `pipeline_nodes * steps`；pass-marker 检测器补齐 `<N>-node` 消息（见 §6） |
| guest 断言 | `run_llm_infer_eight_node_guest.sh` | 3 处正则 `pipeline_nodes=8` → `$SIM_W5_CLUSTER_NODE_COUNT`；终末节点判断 `idx == 8` → `idx == $SIM_W5_CLUSTER_NODE_COUNT` |
| 契约测试 | `tests/test_w5_cluster_topology.py` | 4 进入合法节点集（非法值改为 5），launcher 4 节点映射断言 |

host 侧 `balanced_layer_ranges()`（sim-models qwen3_dense）本就按
`profile.tp_nodes` 通用均衡切分（含余数处理），无需改动；KV heads
8 % 4 = 0 满足张量并行约束。

## 6. 顺带修复的既有断裂（影响所有节点数）

- **pass-marker 检测器脱节**：`qwen3_guest_w5_pass_marker_present()` 期待
  字面量 `eight-node w5 inference cluster validation passed`，而 guest 脚本
  自 `7605d5f`（2026-07-12，DeepSeek 拓扑无关化）起打印
  `<N>-node W5 inference cluster validation passed`。结果是该入口自 7 月 12 日
  起**对所有节点数均以 exit 1 结束**（本次 8 节点基线实测复现）。已修
  （保留旧字面量兼容测试 fixture）。
  修复后 8 节点复跑同样 exit 0（`pass=true`、range_forwards=32/32、
  4 token 与 4 节点逐字一致）——修复对两种拓扑同时生效。
- **期望计数硬编码**：`qwen3_guest_expected_worker_counts()` 的 `8 * steps`
  使任何非 8 节点运行必挂 `worker incomplete`。已参数化。

## 7. 测试证据

- `cargo test --workspace`：225 通过 / 1 失败——`lingqu_memory_publish_paper_engram_state_ref_cli_runs`
  为预先存在（`git stash` 后基线复跑同样失败，实证）
- `python3 -m unittest discover guest-linux/aarch64/tests`：229 项，
  4 个 error 为既有环境问题（`/private/tmp` macOS 路径 ×3、pto-isa host
  g++ `-Werror` ×1，改动前基线实证）
- 契约测试 `test_w5_cluster_topology.py` 9/9 绿（含新的 4 节点断言）
- Rust 单测新增 `(steps=4, nodes=4) → 16/15/16` 期望计数用例

## 8. 已知噪音与遗留

- prepare 期 `qwen3 Object Service payload index is missing` 警告：**两种拓扑
  均出现**（8 节点基线同样打印），store 的 `.bin` sidecar 由运行期写入晚于
  prepare 检查；不阻塞判定（guest PASS / 退出码 0）。可作为后续项把该检查
  移到运行后或容忍"store 由 runtime 创建"分支。
- 命名遗留：summary 文件名前缀 `eight_node_*`、`worker_path: 8-node ...`
  标签、脚本名 `run_llm_infer_eight_node_guest.sh`——均为历史命名，实际已
  拓扑无关。改名影响面大，暂不动。
- W5 profile 表 `W5_INFERENCE_PROFILE_SPECS.nodes: 8` 字段仅信息展示，
  与实际集群规模解耦（实际由 `SIM_W5_CLUSTER_NODE_COUNT` 决定）。

## 9. 产物路径

- 运行日志：`guest-linux/aarch64/logs/2026-08-20_18-36-52_w5_qwen3_0_6b_decode_19412_headless8/`（nodeA-D guest/qemu 日志）
- 汇总：`guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-08-20_18-36-52_w5_qwen3_0_6b_decode_19412.txt`
- Memory 边界观测：`guest-linux/aarch64/out/w5_memory_runtime_boundary_lookup.2026-08-20_18-36-52_w5_qwen3_0_6b_decode_19412.json`
- 对象存储：`guest-linux/aarch64/out/w5_object_service_store.2026-08-20_18-36-52_*.json/.bin`
- 主日志留档：`/tmp/w5_final.log`（4 节点）、`/tmp/w5_8node_base.log`（8 节点基线）
