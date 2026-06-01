# Qwen3 Guest Simpler W5 使用指南

本文说明如何在 `ub_sim` 仓内使用已打包的 `build_output/Qwen*` programs，运行 Qwen3-0.6B / Qwen3-14B 的 8-node QEMU guest + W5 Continue + simpler L2 上卡生成。

这条链路面向多节点多卡功能验证:

- 单卡单节点入口仍是 `qwen3-simpler-generate`。
- 8-node QEMU guest + simpler 入口是 `qwen3-guest-simpler-generate`。
- 当前 guest simpler 主线只支持 W5 Continue，不再暴露旧 W4 fallback 用户参数。
- 当前验证模型为 Qwen3-0.6B 和 Qwen3-14B。
- 当前验证约束为 batch=1、greedy、`max_seq_len=512`。

## 运行前准备

在具备 Ascend/simpler runtime 的机器或容器内进入 `ub_sim` 仓库根目录:

```bash
cd /path/to/ub_sim
git submodule update --init vendor/simpler vendor/pto-isa
```

需要本机提供模型权重目录。本文命令使用以下路径，可按实际环境替换:

```text
/data/models/Qwen/qwen3-0.6B
/data/models/Qwen/Qwen3-14B
```

8-node 验证推荐显式指定 device ids，不依赖 `ASCEND_RT_VISIBLE_DEVICES`:

```bash
unset ASCEND_RT_VISIBLE_DEVICES
```

本文命令使用 8-15 卡:

```text
nodeA -> device 8
nodeB -> device 9
nodeC -> device 10
nodeD -> device 11
nodeE -> device 12
nodeF -> device 13
nodeG -> device 14
nodeH -> device 15
```

## 两种执行模式

`qwen3-guest-simpler-generate` 通过 `--range-exec` 选择 node 内执行方式。

```text
--range-exec single-layer   # per-layer 模式，每个 node 内逐层 dispatch
--range-exec merged-range   # range 模式，每个 node 内一个 layer range 一次 dispatch
```

per-layer 模式:

- 每个 node 按自己的 layer range，逐层调用 single-layer prefill/decode program。
- 这是兼容性和数值回退路径。
- 只依赖命令行传入的四个 L2 build_output 目录。

range 模式:

- 每个 node 按自己的 layer range，一次调用 merged prefill/decode program。
- QEMU/W5/OBMM 外层数据流不变，区别只在 range worker 内部执行排布。
- 用户命令仍传 single-layer 四目录；worker 会根据模型和本 node `layer_count` 自动选择仓内 merged-range program。
- merged-range program 目录当前不是用户参数，而是仓内约定目录；因此运行 `--range-exec merged-range` 时，相关 `build_output/Qwen*Merged*` 目录必须存在于仓库根目录的 `build_output/` 下。
- Qwen3-0.6B 的 8-node layer range 是 `4,4,4,4,3,3,3,3`，因此仓内提供 range 4 和 range 3 merged programs。
- Qwen3-14B 的 8-node layer range 是 `5,5,5,5,5,5,5,5`，因此仓内提供 range 5 merged programs。

## Build Output 对照

Qwen3-0.6B per-layer L2:

```text
build_output/Qwen306BPrefillProgram_20260522_120108
build_output/Qwen3Decode_20260522_120109
build_output/Qwen3FinalRMS_20260522_120109
build_output/Qwen3LMHead_20260522_120110
```

Qwen3-0.6B range L2:

```text
build_output/Qwen306BMergedPrefillRange4
build_output/Qwen306BMergedDecodeRange4
build_output/Qwen306BMergedPrefillRange3
build_output/Qwen306BMergedDecodeRange3
```

自动选择规则:

```text
nodeA/B/C/D layer_count=4 -> Qwen306BMergedPrefillRange4 + Qwen306BMergedDecodeRange4
nodeE/F/G/H layer_count=3 -> Qwen306BMergedPrefillRange3 + Qwen306BMergedDecodeRange3
```

Qwen3-14B per-layer L2:

```text
build_output/Qwen314BPrefillProgram_20260529_053315
build_output/Qwen3Decode_20260529_053316
build_output/Qwen3FinalRMS_20260529_053316
build_output/Qwen3LMHead_20260529_053316
```

Qwen3-14B range L2:

```text
build_output/Qwen314BMergedPrefillRange5
build_output/Qwen314BMergedDecodeRange5
```

自动选择规则:

```text
nodeA/B/C/D/E/F/G/H layer_count=5 -> Qwen314BMergedPrefillRange5 + Qwen314BMergedDecodeRange5
```

## Qwen3-0.6B per-layer

用途:

- 验证 0.6B 的逐层 dispatch 回退路径。
- 适合排查 merged-range 数值差异，或确认 single-layer build_output 可用。

命令:

```bash
cd /path/to/ub_sim
unset ASCEND_RT_VISIBLE_DEVICES
PYTHON=/usr/bin/python3 \
UB_GUEST_ARTIFACT_SOURCE=none \
RECONFIGURE=0 \
QEMU_BUILD_JOBS=32 \
QEMU_CONFIGURE_ARGS="--disable-werror --disable-docs" \
SIM_QEMU_UB_FILTER_DEBUG_LOGS=1 \
SIM_QWEN3_SIMPLER_REAL_DISPATCH_STRICT=1 \
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate \
  --weights-path /data/models/Qwen/qwen3-0.6B \
  --steps 10 \
  --prompt "Huawei is" \
  --platform a2a3 \
  --device-ids 8,9,10,11,12,13,14,15 \
  --decode-abi single-layer \
  --range-exec single-layer \
  --prefill-build-output build_output/Qwen306BPrefillProgram_20260522_120108 \
  --decode-build-output build_output/Qwen3Decode_20260522_120109 \
  --final-rms-build-output build_output/Qwen3FinalRMS_20260522_120109 \
  --lm-head-build-output build_output/Qwen3LMHead_20260522_120110 \
  --profile-verbose
```

10-token 预期:

```text
terminal_tokens=[264, 3644, 7653, 304, 279, 2070, 315, 1995, 5440, 11]
text=" a global leader in the field of information technology,"
```

## Qwen3-0.6B range

用途:

- 验证 0.6B 的 range merged dispatch 路径。
- 外层 W5 数据流与 per-layer 相同，node 内由 merged-range program 执行。
- 命令不需要额外传 `Qwen306BMerged*` 目录；worker 会按 0.6B 的 layer count 自动选择 range 4 或 range 3 program。

命令:

```bash
cd /path/to/ub_sim
unset ASCEND_RT_VISIBLE_DEVICES
PYTHON=/usr/bin/python3 \
UB_GUEST_ARTIFACT_SOURCE=none \
RECONFIGURE=0 \
QEMU_BUILD_JOBS=32 \
QEMU_CONFIGURE_ARGS="--disable-werror --disable-docs" \
SIM_QEMU_UB_FILTER_DEBUG_LOGS=1 \
SIM_QWEN3_SIMPLER_REAL_DISPATCH_STRICT=1 \
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate \
  --weights-path /data/models/Qwen/qwen3-0.6B \
  --steps 10 \
  --prompt "Huawei is" \
  --platform a2a3 \
  --device-ids 8,9,10,11,12,13,14,15 \
  --decode-abi single-layer \
  --range-exec merged-range \
  --prefill-build-output build_output/Qwen306BPrefillProgram_20260522_120108 \
  --decode-build-output build_output/Qwen3Decode_20260522_120109 \
  --final-rms-build-output build_output/Qwen3FinalRMS_20260522_120109 \
  --lm-head-build-output build_output/Qwen3LMHead_20260522_120110 \
  --profile-verbose
```

10-token 预期:

```text
terminal_tokens=[264, 3644, 7653, 304, 279, 2070, 315, 1995, 5440, 11]
text=" a global leader in the field of information technology,"
```

## Qwen3-14B per-layer

用途:

- 验证 14B 的逐层 dispatch 回退路径。
- 14B per-layer 路径更慢，建议先跑 `--steps 3`；需要完整 smoke 时再把 `--steps` 改成 `10`。

命令:

```bash
cd /path/to/ub_sim
unset ASCEND_RT_VISIBLE_DEVICES
rm -rf /tmp/w5_no_reuse_14b_per_layer && mkdir -p /tmp/w5_no_reuse_14b_per_layer
PYTHON=/usr/bin/python3 \
UB_GUEST_ARTIFACT_SOURCE=none \
RECONFIGURE=0 \
QEMU_BUILD_JOBS=32 \
QEMU_CONFIGURE_ARGS="--disable-werror --disable-docs" \
SIM_W5_MEMORY_REUSE_OUT_DIR=/tmp/w5_no_reuse_14b_per_layer \
SIM_QEMU_UB_FILTER_DEBUG_LOGS=1 \
SIM_QWEN3_SIMPLER_REAL_DISPATCH_STRICT=1 \
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate \
  --weights-path /data/models/Qwen/Qwen3-14B \
  --steps 3 \
  --prompt "Huawei is" \
  --platform a2a3 \
  --device-ids 8,9,10,11,12,13,14,15 \
  --decode-abi single-layer \
  --range-exec single-layer \
  --prefill-build-output build_output/Qwen314BPrefillProgram_20260529_053315 \
  --decode-build-output build_output/Qwen3Decode_20260529_053316 \
  --final-rms-build-output build_output/Qwen3FinalRMS_20260529_053316 \
  --lm-head-build-output build_output/Qwen3LMHead_20260529_053316 \
  --profile-verbose
```

3-token 预期:

```text
terminal_tokens=[264, 2813, 429]
text=" a company that"
```

如果要跑 10-token，把 `--steps 3` 改为 `--steps 10`。14B per-layer 10-token 输出可能和 range 路径不逐 token 对齐，做回归时应记录当前实际 token/text，不要和 merged-range golden 混用。

## Qwen3-14B range

用途:

- 验证 14B 的 range merged dispatch 主路径。
- 当前 14B range smoke 已验证 10-token 生成。
- 命令不需要额外传 `Qwen314BMerged*` 目录；worker 会按 14B 的 layer count 自动选择 range 5 program。

命令:

```bash
cd /path/to/ub_sim
unset ASCEND_RT_VISIBLE_DEVICES
rm -rf /tmp/w5_no_reuse_14b_range && mkdir -p /tmp/w5_no_reuse_14b_range
PYTHON=/usr/bin/python3 \
UB_GUEST_ARTIFACT_SOURCE=none \
RECONFIGURE=0 \
QEMU_BUILD_JOBS=32 \
QEMU_CONFIGURE_ARGS="--disable-werror --disable-docs" \
SIM_W5_MEMORY_REUSE_OUT_DIR=/tmp/w5_no_reuse_14b_range \
SIM_QEMU_UB_FILTER_DEBUG_LOGS=1 \
SIM_QWEN3_SIMPLER_REAL_DISPATCH_STRICT=1 \
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate \
  --weights-path /data/models/Qwen/Qwen3-14B \
  --steps 10 \
  --prompt "Huawei is" \
  --platform a2a3 \
  --device-ids 8,9,10,11,12,13,14,15 \
  --decode-abi single-layer \
  --range-exec merged-range \
  --prefill-build-output build_output/Qwen314BPrefillProgram_20260529_053315 \
  --decode-build-output build_output/Qwen3Decode_20260529_053316 \
  --final-rms-build-output build_output/Qwen3FinalRMS_20260529_053316 \
  --lm-head-build-output build_output/Qwen3LMHead_20260529_053316 \
  --profile-verbose
```

10-token 预期:

```text
terminal_tokens=[264, 8453, 67926, 5440, 2813, 429, 14431, 323, 30778, 11502]
text=" a Chinese multinational technology company that designs and sells consumer"
```

## 参数说明

必需参数:

- `--weights-path`: Qwen3 模型权重目录。
- `--steps`: 生成 token 数。0.6B 常用 `10`；14B per-layer 建议先用 `3`。
- `--prompt`: prompt 文本。
- `--platform`: simpler 平台，当前验证为 `a2a3`。
- `--device-ids`: 8 个 node 对应的 device ids，格式如 `8,9,10,11,12,13,14,15`。
- `--decode-abi single-layer`: 当前 guest simpler L2 使用 single-layer decode ABI。
- `--range-exec`: `single-layer` 或 `merged-range`。
- 四个 `--*-build-output`: single-layer L2 prefill/decode/final_rms/lm_head program 目录。

`merged-range` 目录说明:

- 当前没有 `--merged-prefill-build-output` 或 `--merged-decode-build-output` 参数。
- merged-range 目录通过仓内固定命名自动解析，解析依据是模型规格和当前 node 的 `layer_count`。
- 若要移动、重命名或新增其它 range size 的 merged program，需要同步更新 worker 内的目录选择规则。

常用环境变量:

- `SIM_QWEN3_SIMPLER_REAL_DISPATCH_STRICT=1`: dispatch 失败时严格报错。
- `SIM_QEMU_UB_FILTER_DEBUG_LOGS=1`: 人工验证时过滤高频 QEMU/UB debug log。脚本默认是 `0`。
- `SIM_W5_MEMORY_REUSE_OUT_DIR=/tmp/<isolated-dir>`: 隔离 W5 memory reuse。跨模型验证，尤其是 14B，建议设置。
- `UB_GUEST_ARTIFACT_SOURCE=none`: 使用当前仓内已构建 artifact。
- `RECONFIGURE=0`: 不强制重新 configure QEMU。

## 日志与调试

脚本默认不启用 QEMU/UB debug log 过滤:

```text
SIM_QEMU_UB_FILTER_DEBUG_LOGS=0
```

如果要做性能日志收集、长 steps 验证，或者按本文 smoke 命令做人工验收，建议显式打开:

```bash
SIM_QEMU_UB_FILTER_DEBUG_LOGS=1 \
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate ...
```

打开后，`launch_ub_eight_node_headless.sh` 会过滤高频 UB/QEMU debug 行，显著降低 log volume。它只影响日志输出，不改变 Qwen3 W5/simpler 语义。因为它会改变默认日志可见性，所以仓库默认保持关闭，测试脚本或人工验证时按需显式置 1。

常用验证后清理:

```bash
pkill -TERM -f 'sim-cli|qwen3-simpler-range-worker|qemu-system-aarch64' || true
sleep 2
pkill -KILL -f 'sim-cli|qwen3-simpler-range-worker|qemu-system-aarch64' || true
ps -ef | grep -E 'sim-cli|qwen3-simpler-range-worker|qemu-system-aarch64' | grep -v grep || true
```

## 静态和单元测试

```bash
cargo fmt --check -p sim-cli -p sim-uapi -p sim-memory -p sim-chipbackend-simpler -p sim-models
cargo test -p sim-cli qwen3_guest_simpler -- --nocapture
cargo test -p sim-uapi qwen3_simpler -- --nocapture
cargo test -p sim-chipbackend-simpler
cargo test -p sim-models qwen3 -- --nocapture
cargo test -p sim-memory -- --nocapture
python3 -m pytest guest-linux/aarch64/tests/test_qwen3_dense_env.py -q
```

如果修改了 guest C 或 QEMU/initramfs 相关代码，需要重新构建 guest artifact 后再跑 smoke。
