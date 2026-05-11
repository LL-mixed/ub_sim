# 8-node W4 Qwen3 guest decode loop 运行环境清单

本文定义 8-node W4 Qwen3 guest decode-loop 的标准运行参数来源与默认值。目标是让任何人只改一个文档就能知道：

1) 要设什么，2) 默认值是什么，3) 该值在哪一层生效。

## 运行入口

命令示例：

```bash
SIM_QWEN3_0_6B_WEIGHTS_PATH=/path/to/Qwen3-0.6B \
  cargo run --release -p sim-cli -- qwen3-guest-decode-loop \
  --steps 16 \
  --prompt "Capital of China is" \
  --matmul-batch 4
```

默认会执行 `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh`。

## 重要默认值总表

| 层级 | 变量 | 默认值 | 作用 | 生效脚本/代码 |
|---|---|---|---|---|
| sim-cli | `SIM_QWEN3_0_6B_WEIGHTS_PATH` | 无默认（必需） | Qwen3 权重目录 | 用户环境（调用方） |
| sim-cli | `SIM_QWEN3_TEMPERATURE` | `qwen3-guest-decode-loop` 默认不设置 | 采样温度 | 当前 `qwen3-guest-decode-loop` 不会自动注入；如需需外部手工设置 |
| sim-cli | `SIM_QWEN3_ROUND1_DISPATCH_BATCH` | 不设置（未显式传 `--matmul-batch`） | round1 matmul dispatch 批大小 | `prepare_qwen3_matmul_batch_environment` |
| sim-cli | `SIMPLER_HOST_MATMUL_BATCH_MANIFEST` | 无默认，按 batch 动态生成 | 8-node batch matmul 入口 manifest | `prepare_qwen3_matmul_batch_environment` |
| sim-cli | `SIMPLER_HOST_MATMUL_MANIFEST` | `/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json` | Host 端 matmul base manifest | `prepare_qwen3_matmul_batch_environment` |
| sim-cli | `SIM_UAPI_W4_CHIPBACKEND_PROFILE` | `qwen3_dense_0_6b` | 使 guest 走 qwen3 0.6B 运行路径 | `run_qwen3_guest_decode_loop_cli` |
| CLI | `--max-token` / `--steps` | 1 | `qwen3-guest-decode-loop` 步数（每轮 token） | `qwen3_guest_decode_loop_args_from` |
| CLI | `--script` | `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh` | 可替换启动脚本 | `default_qwen3_guest_decode_script_path` |
| CLI | `--prompt` | 空字符串 | 首轮 prompt | `qwen3_guest_decode_loop_args_from` |
| CLI | `--matmul-batch` | 不设置 | 触发 batch manifest 与 round1 batch env 生成 | `qwen3_guest_decode_loop_args_from` |
| launch（host） | `QEMU_MEM` | `2G` | 每个 QEMU 节点内存 | `launch_ub_eight_node_headless.sh` |
| launch（host） | `QEMU_SMP` | `2` | 每个 QEMU 节点 vCPU 数 | `launch_ub_eight_node_headless.sh` |
| launch（host） | `APPEND_EXTRA` | `linqu_probe_skip=1 linqu_probe_load_helper=1` | 内核 cmdline 追加参数基础项 | `launch_ub_eight_node_headless.sh` + `ensure_sim_kernel_append_defaults` |
| launch（host） | `SIM_UAPI_W4_CHIPBACKEND_PROFILE` | `host_vector`（被上游脚本改成 `qwen3_dense_0_6b`） | scenario profile 选择 | `launch_ub_eight_node_headless.sh` |
| launch（host） | `SIM_UAPI_SCENARIO_CONFIG` | `scenarios/mvp_8host_single_domain.yaml` | 场景 yaml | `launch_ub_eight_node_headless.sh` |
| launch（host） | `SIMPLER_HOST_MATMUL_MANIFEST` | `/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json`（路径名按本地脚本约定） | host matmul artifact | `launch_ub_eight_node_headless.sh` |
| run（guest wrapper） | `SIMPLER_HOST_MATMUL_MANIFEST` | `/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json` | guest 启动时 fallback manifest | `run_ub_eight_node_w4_guest.sh` |
| run（guest wrapper） | `SIM_UAPI_W4_CHIPBACKEND_PROFILE` | `qwen3_dense_0_6b` | guest/节点工作流分支 | `run_ub_eight_node_w4_guest.sh` |
| run（guest wrapper） | `SIM_QWEN3_GUEST_DECODE_STEPS` | `1` | 每个 node decode 步数上限 | `run_ub_eight_node_w4_guest.sh` |
| run（guest wrapper） | `SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS` | `9707,1207,16948,18` | prompt token 版本（脚本会覆盖） | `run_ub_eight_node_w4_guest.sh` |
| run（guest wrapper） | `SIM_W4_UAPI_COMPLETION_TIMEOUT_MS` | `900000` | completion 超时 | `run_ub_eight_node_w4_guest.sh` |
| run（guest wrapper） | `SIM_W4_RESOURCE_ASSERTIONS` | `0` | 资源断言严格度 | `run_ub_eight_node_w4_guest.sh` |
| guest 内核 cmdline | `linqu_probe_skip` | `1` | 启动时跳过部分探测 | `launch_ub_eight_node_headless.sh` |
| guest 内核 cmdline | `linqu_probe_load_helper` | `1` | 加载 helper 行为 | `launch_ub_eight_node_headless.sh` |
| guest 内核 cmdline | `obmm.skip_cache_maintain` | `1` | 避免额外维护开销 | `ensure_sim_kernel_append_defaults` |
| guest 内核 cmdline | `rcupdate.rcu_cpu_stall_timeout` | `300` | RCU stall 超时阈值 | `ensure_sim_kernel_append_defaults` |
| guest 内核 cmdline | `linqu_ipourma_ipv4` | 节点 IP（10.0.0.1~10.0.0.8） | 每个 node 的网络地址 | `launch_ub_eight_node_headless.sh` |
| guest 运行时导出 | `LINQU_UB_ROLE` | nodeA~nodeH | 节点身份 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `LINQU_UB_LOCAL_IP` | 节点 IP | 节点网口配置 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `LINQU_UB_ALL_IPS` | 8-node 全量 IP csv | 节点发现/通信范围 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `LINQU_UB_NODE_COUNT` | `8` | 节点总数 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `LINQU_W4_DB_CLUSTER` | `1` | W4 DB cluster 启用 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `LINQU_W4_REQUIRE_UAPI_RESOURCE` | `1` | 强制 resource-backed 运行路径 | `run_ub_eight_node_w4_guest.sh` |
| guest 运行时导出 | `SIM_W4_DB_LAZY_REMOTE_ACTIVATION` | `1` | 远端对象 lazy 激活 | `run_ub_eight_node_w4_guest.sh` |

## 推荐最小配置（功能跑通）

- `SIM_QWEN3_0_6B_WEIGHTS_PATH`：必须设置
- `QEMU_MEM=2G`、`QEMU_SMP=2`：当前配置可用（8-node）
- `SIM_QWEN3_GUEST_DECODE_STEPS`：按需求设置（例如 4/16）
- `SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense_0_6b`：固定此值
- `SIMPLER_HOST_MATMUL_MANIFEST`：按本机 `prepare` 脚本输出路径设置
- `APPEND_EXTRA`：至少包含 `linqu_probe_skip=1 linqu_probe_load_helper=1 obmm.skip_cache_maintain=1 rcupdate.rcu_cpu_stall_timeout=300`

## 配置传播（可复核）

- sim-cli 先组装 env 并执行 `run_ub_eight_node_w4_guest.sh`。
- `run_ub_eight_node_w4_guest.sh` 再把一组 env 注入到 8 个 guest，并调用 `launch_ub_eight_node_headless.sh`。
- 头less launcher 打印启动日志，例如：
  - `qemu_mem=...`
  - `sim_uapi_scenario_config=...`
  - `append_extra=...`

这三行是判断“实际生效参数”最直接的来源。
