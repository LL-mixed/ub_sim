# Qwen3 Simpler Build Output 验证指南

本文说明如何使用本仓库打包的 `build_output/Qwen*` programs，在 simpler 卡上验证 Qwen3 生成路径。它面向 PR reviewer：验证者只需要 `ub_sim` 仓、可用的模型目录和 simpler/Ascend 运行环境，不需要额外 checkout 或配置 `pypto-lib` 的 build_output 目录。

## 适用范围

- 模型:
  - Qwen3-0.6B
  - Qwen3-14B
- 执行模式:
  - L2: 传四个 build_output program
  - L3: 传一个 `Qwen3GenChunked_*` build_output program，并打开 `--l3`
- 运行目标:
  - 单 host
  - 单 device
  - 已验证 `--platform a2a3 --device-id 0`

模型权重不随本仓库打包。运行时仍需要通过 `--model-dir` 指向本机已有的 Qwen3 模型目录。

## 前置条件

请在具备以下条件的机器或容器中运行:

- Ascend runtime 和可用 NPU device。
- 本仓所需的 simpler runtime 环境。
- 可执行 `cargo run` 的 Rust/Cargo 环境。
- 本机已有 Qwen3 模型目录，例如:

```text
/home/sj/models/Qwen/Qwen3-0.6B/
/home/sj/models/Qwen/Qwen3-14B/
```

所有命令都从 `ub_sim` 仓库根目录执行:

```bash
cd /path/to/ub_sim
```

下文所有 `--build-output` 都使用相对于仓库根目录的 `build_output/Qwen*` 路径。

## 仓内 Build Output

Qwen3-0.6B L3:

```text
build_output/Qwen3GenChunked_20260516_120746
```

Qwen3-0.6B L2:

```text
build_output/Qwen306BPrefillProgram_20260516_120745
build_output/Qwen3Decode_20260516_120745
build_output/Qwen3FinalRMS_20260516_120745
build_output/Qwen3LMHead_20260516_120746
```

Qwen3-14B L3:

```text
build_output/Qwen3GenChunked_20260517_220211
```

Qwen3-14B L2:

```text
build_output/Qwen314BPrefillProgram_20260517_220210
build_output/Qwen3Decode_20260517_220210
build_output/Qwen3FinalRMS_20260517_220211
build_output/Qwen3LMHead_20260517_220211
```

L2 模式必须传齐四个 program。L3 模式必须只传一个 `Qwen3GenChunked_*` 目录，并加上 `--l3`。

## Runtime Manifest

`qwen3-simpler-generate` 首次运行时会自动准备一个可复用的 runtime-only manifest:

```text
/tmp/simpler-qwen3-<runtime>-<platform>-runtime-artifacts/simpler_runtime_manifest.json
```

当前仓内 Qwen3 build output 已验证的 runtime 是 `tensormap_and_ringbuffer`，因此 `a2a3` 平台下的典型路径是:

```text
/tmp/simpler-qwen3-tensormap-and-ringbuffer-a2a3-runtime-artifacts/simpler_runtime_manifest.json
```

这个 manifest 只保存 simpler runtime binary，不保存模型权重，也不保存 Qwen3 program binary。Qwen3 program 始终从命令行传入的 `build_output/Qwen*` 路径读取。

如果 simpler runtime 更新、platform 切换，或怀疑 `/tmp` 下 runtime cache 已陈旧，可以删除对应 `/tmp/simpler-qwen3-*` 目录后重新运行 smoke 命令。

## Qwen3-0.6B Smoke 命令

L3:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate --l3 \
  --build-output build_output/Qwen3GenChunked_20260516_120746 \
  --model-dir /home/sj/models/Qwen/Qwen3-0.6B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 10 \
  --platform a2a3 \
  --device-id 0 \
  --profile-verbose
```

预期最终 stdout:

```text
text:  a company that has been around for over 2
token_ids: [264, 2813, 429, 702, 1012, 2163, 369, 916, 220, 17]
finish_reason: length
```

L2:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate \
  --build-output build_output/Qwen306BPrefillProgram_20260516_120745 \
  --build-output build_output/Qwen3Decode_20260516_120745 \
  --build-output build_output/Qwen3FinalRMS_20260516_120745 \
  --build-output build_output/Qwen3LMHead_20260516_120746 \
  --model-dir /home/sj/models/Qwen/Qwen3-0.6B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 10 \
  --platform a2a3 \
  --device-id 0 \
  --profile-verbose
```

预期最终 stdout:

```text
text:  a global leader in the field of information technology,
token_ids: [264, 3644, 7653, 304, 279, 2070, 315, 1995, 5440, 11]
finish_reason: length
```

## Qwen3-14B Smoke 命令

L3:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate --l3 \
  --build-output build_output/Qwen3GenChunked_20260517_220211 \
  --model-dir /home/sj/models/Qwen/Qwen3-14B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 10 \
  --platform a2a3 \
  --device-id 0 \
  --profile-verbose
```

预期最终 stdout:

```text
text:  a Chinese multinational technology company that designs and sells consumer
token_ids: [264, 8453, 67926, 5440, 2813, 429, 14431, 323, 30778, 11502]
finish_reason: length
```

L2:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate \
  --build-output build_output/Qwen314BPrefillProgram_20260517_220210 \
  --build-output build_output/Qwen3Decode_20260517_220210 \
  --build-output build_output/Qwen3FinalRMS_20260517_220211 \
  --build-output build_output/Qwen3LMHead_20260517_220211 \
  --model-dir /home/sj/models/Qwen/Qwen3-14B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 10 \
  --platform a2a3 \
  --device-id 0 \
  --profile-verbose
```

预期最终 stdout:

```text
text:  a Chinese multinational technology company that designs and sells consumer
token_ids: [264, 8453, 67926, 5440, 2813, 429, 14431, 323, 30778, 11502]
finish_reason: length
```

## Sampling 与 Stop 检查

默认 `--temperature` 为 `0.0`，因此默认走 greedy decode。`temperature > 0` 时会在 host 侧执行随机采样，输出不要求逐次 bit-stable。

下面是 14B L3 sampling smoke 示例:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate --l3 \
  --build-output build_output/Qwen3GenChunked_20260517_220211 \
  --model-dir /home/sj/models/Qwen/Qwen3-14B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 5 \
  --platform a2a3 \
  --device-id 0 \
  --temperature 0.7 \
  --top-k 20 \
  --top-p 0.9 \
  --profile-verbose
```

sampling smoke 的验收标准:

- 命令执行成功。
- `token_ids` 长度不超过 `--max-new-tokens`。
- `finish_reason` 是 `length`、`eos` 或 `stop` 之一。

下面是 0.6B L2 stop smoke 示例:

```bash
cargo run --release -p sim-cli -- qwen3-simpler-generate \
  --build-output build_output/Qwen306BPrefillProgram_20260516_120745 \
  --build-output build_output/Qwen3Decode_20260516_120745 \
  --build-output build_output/Qwen3FinalRMS_20260516_120745 \
  --build-output build_output/Qwen3LMHead_20260516_120746 \
  --model-dir /home/sj/models/Qwen/Qwen3-0.6B/ \
  --prompt "Huawei is" \
  --max-seq-len 512 \
  --max-new-tokens 10 \
  --platform a2a3 \
  --device-id 0 \
  --stop "information technology," \
  --profile-verbose
```

预期最终 stdout:

```text
text:  a global leader in the field of information technology,
token_ids: [264, 3644, 7653, 304, 279, 2070, 315, 1995, 5440, 11]
finish_reason: stop
```

## Profile 输出

最终生成结果固定打印到 stdout:

```text
text: ...
token_ids: [...]
finish_reason: length|eos|stop
```

打开 `--profile-verbose` 后，诊断信息打印到 stderr，包含:

- 选中的 build_output artifact。
- simpler runtime manifest 路径。
- platform 和 device id。
- model spec。
- sampling 配置。
- `runtime buffer: reused across dispatches`。
- prefill 耗时。
- 每个 token 的 decode/project/sample 耗时。
- 总 wall-clock 耗时。

## 输入约束

- batch 固定为 1。
- `--max-seq-len` 必须是 256 的正倍数。
- `prompt_tokens + max_new_tokens <= max_seq_len`。
- `--max-new-tokens > 0`。
- `--top-p` 必须满足 `0 < top_p <= 1`。
- `--top-k 0` 等价于关闭 top-k。
- `--stop` 可重复传入；空 stop string 会被忽略。

## 常见问题排查

模型文件缺失:

- 确认 `--model-dir` 指向完整的 Hugging Face Qwen3 模型目录。
- 该目录需要包含 tokenizer/config 文件和 safetensors 权重。

build_output 文件缺失:

- L2 目录必须包含 `kernel_config.py`、`orchestration/*.so` 和 `kernels/**/*.o`。
- L3 目录必须包含 `orchestration/host_orch.py` 和已编译的 `next_levels/*` artifacts。
- 使用相对 `build_output/Qwen*` 路径时，确认命令从 `ub_sim` 仓库根目录执行。

runtime manifest 陈旧:

```bash
rm -rf /tmp/simpler-qwen3-tensormap-and-ringbuffer-a2a3-runtime-artifacts
```

删除后重新执行同一个 smoke 命令即可。

NPU runtime error:

- 先从 stderr 确认失败进程 id 和 device id。
- 如果当前运行环境生成了 device debug log，优先查看对应 device debug log。
- 再查看验证机器上的 Ascend plog。
- 如果错误像是 runtime artifact 过期，而不是模型或 program 不匹配，可以只清理 `/tmp/simpler-qwen3-*` runtime manifest 后重跑。
