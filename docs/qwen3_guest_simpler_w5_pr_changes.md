# Qwen3 Guest Simpler W5 PR 改动说明

本文概述 Qwen3 guest simpler W5 特性相对 upstream `master` 的改动范围，帮助 reviewer 判断哪些是 Qwen3 专属路径，哪些触及了通用 ub_sim 框架。

## 功能目标

- 新增 `qwen3-guest-simpler-generate`，用于 8-node QEMU guest + W5 Continue + simpler L2 上卡生成。
- 支持 Qwen3-0.6B 和 Qwen3-14B。
- 支持两种 node 内执行方式:
  - `single-layer`: 每层单独 dispatch，作为兼容回退。
  - `merged-range`: 每个 node layer range 一次 dispatch，用于减少 worker 内 launch 次数。
- 保留单卡单节点 `qwen3-simpler-generate` 路径。

## 主要代码改动

### CLI

- `crates/sim-cli/src/main.rs`
  - 增加 `qwen3-guest-simpler-generate` 参数解析。
  - 增加 `--device-ids`、`--range-exec`、四个 L2 build_output 参数。
  - 将 guest simpler 默认 profile 设为 `qwen3_guest_simpler_w5_l2`。
  - 禁止 guest simpler 入口继续使用 `--script` 切回旧 W4 fallback。

### Qwen3 simpler runner

- `crates/sim-uapi/src/qwen3_simpler.rs`
  - 从 `sim-cli` 移入 `sim-uapi`，供 guest/UAPI 路径复用。
  - 实现 Qwen3 L2 build_output loader、tensor 准备、range dispatch、terminal projection、sampling。
  - 实现 long-lived range worker cache，复用 runtime/context/prepared callable。
  - 实现 `RangeExec::SingleLayer` 和 `RangeExec::MergedRange`。

### W5/UAPI 接入

- `crates/sim-uapi/src/lib.rs`
  - 将 `qwen3_guest_simpler_w5_l2` 接入 W4 chipbackend profile。
  - 支持 W5 object/ref 形式的 range input/output。
  - 支持 nodeH 执行 final_rms/lm_head/sample 并写回 terminal token object。

### simpler runtime wrapper

- `crates/sim-chipbackend-simpler/src/lib.rs`
  - 适配新版 simpler C API。
  - 支持指定 device id。
  - 支持 `run_runtime` 和 prepared callable API。

- `crates/sim-runtime/src/lib.rs`
  - 使用指定 device id 创建 simpler context。
  - 使用新版 runtime launch API。

### 模型元信息

- `crates/sim-models/src/qwen3_dense_reference.rs`
  - 补充 Qwen3 0.6B/14B 运行时需要的模型元信息、权重 shape 与采样辅助数据。

### guest 脚本和 C 侧服务

- `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh`
  - 接入 `qwen3_guest_simpler_w5_l2` profile。
  - 校验 build_output 目录和 required artifacts。
  - 透传 Qwen3 simpler env。

- `guest-linux/aarch64/scripts/launch_ub_eight_node_headless.sh`
  - 透传 Qwen3 simpler env。
  - 增加可选 QEMU/UB debug log filter。
  - `SIM_QEMU_UB_FILTER_DEBUG_LOGS` 默认值保持 `0`，测试时按需显式置 `1`。

- `guest-linux/aarch64/scripts/qemu_ub_common.sh`
  - 增加 opt-in `UB_SKIP_QEMU_BUILD=1`，允许复用已有 QEMU binary。

- `guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh`
  - 允许 W5 profile 选择 `qwen3_guest_simpler_w5_l2`。

- `guest-linux/aarch64/scripts/run_w5_cluster_config.sh`
  - 增加 Qwen3 guest simpler profile 配置支持。

- `guest-linux/aarch64/w4_guest_qemu_demo.c`
  - 增加 Qwen3 W5 range task、hidden/KV/token 流转支持。

- `guest-linux/aarch64/w4_kvcache_db_service.c`
  - 增加 Qwen3 terminal token owner hint。
  - 增加 range wait 细分打点。
  - 修正 prefill hidden handoff bytes 计算。
  - 对 Qwen3 simpler profile 支持 range-only service coverage，减少无关 legacy UAPI coverage。

### 测试

- `guest-linux/aarch64/tests/test_qwen3_dense_env.py`
  - 增加 Qwen3 guest simpler profile/env 的 source-level guards。

## 打包的 build_output

新增以下最小验证 programs:

- Qwen3-0.6B single-layer:
  - `build_output/Qwen306BPrefillProgram_20260522_120108`
  - `build_output/Qwen3Decode_20260522_120109`
  - `build_output/Qwen3FinalRMS_20260522_120109`
  - `build_output/Qwen3LMHead_20260522_120110`
- Qwen3-0.6B merged-range:
  - `build_output/Qwen306BMergedPrefillRange4`
  - `build_output/Qwen306BMergedDecodeRange4`
  - `build_output/Qwen306BMergedPrefillRange3`
  - `build_output/Qwen306BMergedDecodeRange3`
- Qwen3-14B single-layer:
  - `build_output/Qwen314BPrefillProgram_20260529_053315`
  - `build_output/Qwen3Decode_20260529_053316`
  - `build_output/Qwen3FinalRMS_20260529_053316`
  - `build_output/Qwen3LMHead_20260529_053316`
- Qwen3-14B merged-range:
  - `build_output/Qwen314BMergedPrefillRange5`
  - `build_output/Qwen314BMergedDecodeRange5`

这些目录只包含 runtime 必需文件: `kernel_config.py`、`orchestration/*.so`、`kernels/**/*.o`。

## 子模块

- `vendor/pto-isa` gitlink 更新到已验证 commit。

这样 reviewer 使用 `git submodule update --init vendor/simpler vendor/pto-isa` 后，可以获得和验证环境一致的 pto-isa/simpler 依赖。

## 默认框架行为影响

本 PR 尽量把 Qwen3 guest simpler 行为限制在显式 profile 下:

```text
SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_guest_simpler_w5_l2
```

需要特别注意的通用层改动:

- `sim-chipbackend-simpler` 和 `sim-runtime` 是通用 simpler runtime wrapper。它们为新版 simpler API 和 device-id 支持做了兼容改造，可能影响其它 simpler-backed 路径。
- `w4_kvcache_db_service.c` 是 W4/W5/OBMM 通用服务，但 Qwen3 terminal hint、range-only coverage 等路径均由 `qwen3_guest_simpler_w5_l2` profile 或 Qwen3 env gate 触发。
- `SIM_QEMU_UB_FILTER_DEBUG_LOGS` 默认保持 `0`，不会改变默认 QEMU 日志输出。做长 steps 或性能验证时建议显式置 `1`。
- `UB_SKIP_QEMU_BUILD=1` 是 opt-in，不改变默认 QEMU build 行为。

因此，默认 `host_vector`、`host_matmul`、`qwen3_dense`、Engram/W5 既有路径不应因为不设置 Qwen3 guest simpler profile 而改变用户可见行为。
