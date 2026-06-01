# Qwen3 Guest Simpler W5 使用指南

本文说明如何在 `ub_sim` 仓内使用已打包的 `build_output/Qwen*` programs，运行 Qwen3-0.6B / Qwen3-14B 的 8-node QEMU guest + W5 Continue + simpler L2 上卡生成。

这条链路面向多节点多卡功能验证:

- 单卡单节点入口仍是 `qwen3-simpler-generate`。
- 8-node QEMU guest + simpler 入口是 `qwen3-guest-simpler-generate`。
- 当前 guest simpler 主线只支持 W5 Continue，不再暴露旧 W4 fallback 用户参数。
- 当前验证模型为 Qwen3-0.6B 和 Qwen3-14B，batch=1，greedy，`max_seq_len=512`。

## 执行环境

在具备 Ascend/simpler runtime 的机器或容器内进入 `ub_sim` 仓库根目录:

```bash
cd /path/to/ub_sim
git submodule update --init vendor/simpler vendor/pto-isa
```

需要本机提供模型权重目录，例如:

```text
/data/models/Qwen/qwen3-0.6B
/data/models/Qwen/Qwen3-14B
```

8-node 验证推荐显式指定 device ids，不依赖 `ASCEND_RT_VISIBLE_DEVICES`:

```bash
unset ASCEND_RT_VISIBLE_DEVICES
```

本项目验证时使用 8-15 卡:

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

## Build Output 目录

Qwen3-0.6B single-layer L2:

```text
build_output/Qwen306BPrefillProgram_20260522_120108
build_output/Qwen3Decode_20260522_120109
build_output/Qwen3FinalRMS_20260522_120109
build_output/Qwen3LMHead_20260522_120110
```

Qwen3-0.6B merged-range L2:

```text
build_output/Qwen306BMergedPrefillRange4
build_output/Qwen306BMergedDecodeRange4
build_output/Qwen306BMergedPrefillRange3
build_output/Qwen306BMergedDecodeRange3
```

Qwen3-14B single-layer L2:

```text
build_output/Qwen314BPrefillProgram_20260529_053315
build_output/Qwen3Decode_20260529_053316
build_output/Qwen3FinalRMS_20260529_053316
build_output/Qwen3LMHead_20260529_053316
```

Qwen3-14B merged-range L2:

```text
build_output/Qwen314BMergedPrefillRange5
build_output/Qwen314BMergedDecodeRange5
```

用户命令始终传 single-layer 四目录。`--range-exec merged-range` 时，worker 会根据模型和本 node 的 layer range 自动选择对应 merged-range program；若选择 `--range-exec single-layer`，则使用四目录中的 prefill/decode program 逐层 dispatch。

## 执行模式

`--range-exec single-layer`:

- 每个 node 内按 layer 逐层调用 single-layer prefill/decode program。
- 作为兼容性和数值回退路径保留。

`--range-exec merged-range`:

- 每个 node 内按本 node 的 layer range 一次 dispatch merged prefill/decode program。
- QEMU/W5/OBMM 外层数据流不变，区别只在 range worker 内部执行排布。
- 当前 0.6B 的 layer range 为 `4,4,4,4,3,3,3,3`，因此需要 range 4 和 range 3 的 merged programs。
- 当前 14B 的 layer range 为 `5,5,5,5,5,5,5,5`，因此需要 range 5 的 merged programs。

## Qwen3-0.6B merged-range smoke

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

预期:

```text
terminal_tokens=[264, 3644, 7653, 304, 279, 2070, 315, 1995, 5440, 11]
text=" a global leader in the field of information technology,"
```

## Qwen3-14B merged-range smoke

14B 验证建议隔离 W5 memory reuse 目录，避免误用其它模型的 decision/object store:

```bash
cd /path/to/ub_sim
unset ASCEND_RT_VISIBLE_DEVICES
rm -rf /tmp/w5_no_reuse_14b && mkdir -p /tmp/w5_no_reuse_14b
PYTHON=/usr/bin/python3 \
UB_GUEST_ARTIFACT_SOURCE=none \
RECONFIGURE=0 \
QEMU_BUILD_JOBS=32 \
QEMU_CONFIGURE_ARGS="--disable-werror --disable-docs" \
SIM_W5_MEMORY_REUSE_OUT_DIR=/tmp/w5_no_reuse_14b \
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

预期:

```text
terminal_tokens=[264, 8453, 67926, 5440, 2813, 429, 14431, 323, 30778, 11502]
text=" a Chinese multinational technology company that designs and sells consumer"
```

## single-layer fallback smoke

把 `--range-exec merged-range` 改成 `--range-exec single-layer` 即可走逐层 dispatch 回退路径。建议先用较短 steps 验证:

```bash
cargo run --release -p sim-cli -- qwen3-guest-simpler-generate \
  --weights-path /data/models/Qwen/qwen3-0.6B \
  --steps 3 \
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
