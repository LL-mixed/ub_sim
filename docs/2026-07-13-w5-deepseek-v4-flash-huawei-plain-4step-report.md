# W5 DeepSeek V4 Flash `Huawei is` 4-step 运行报告

日期：2026-07-13

最终 run id：`2026-07-13_12-59-26_w5_deepseek_v4_flash_decode_6277`

## 1. 结论

本次使用 8 个 QEMU guest，以原始文本 `Huawei is` 对应的纯文本 BPE token
序列运行 W5 DeepSeek V4 Flash 4-step stream infer。运行结果为：

- 8/8 节点通过；
- 4/4 decode steps 完成；
- 32/32 节点 range 执行记录完整；
- 28/28 跨节点 hidden handoff 完整；
- step 1 到 step 3 的 24 次节点本地 KV restore 全部成功；
- 最终状态为 `PASS`；
- 流式输出为 ` a leading global information`。

本次没有 prefix-cache 或 serving shortpath 命中。4 个 step 均执行了完整的
43-layer、8-node DeepSeek 路径，因此这是连续 decode 正确性验证，不是 cache
收益测试。

## 2. Prompt 完整性

用户提供的文本严格保持为：

```text
Huawei is
```

DeepSeek BPE 对该文本的编码为：

```text
42,5207,24063,344
```

本次最终运行直接使用这 4 个模型 token。没有添加 BOS、`User`、`Assistant`、
`think`、换行、空格或其他模板 token。

运行使用的临时配置核心内容如下：

```text
SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode
SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash-simpler
SIM_QWEN3_GUEST_DECODE_STEPS=4
SIM_LLM_INFER_PROMPT_TOKEN_IDS=42,5207,24063,344
SIM_QWEN3_GUEST_ENGRAM=0
SIM_W5_MEMORY_SERVICE=lingqu_memory_service
QEMU_MEM=8G
QEMU_SMP=2
```

稳定运行入口为：

```sh
./guest-linux/aarch64/scripts/run_w5_in_container.sh \
  /private/tmp/w5.deepseek-v4-flash-huawei-plain-4step.env
```

### 2.1 被拒绝的第一次运行

第一次运行的 run id 为
`2026-07-13_11-44-09_w5_deepseek_v4_flash_decode_4886`。该运行虽然通过了
4/4 steps 和 8/8 节点校验，但现有 raw-prompt 入口固定调用 chat tokenizer，
实际输入为 8 tokens：

```text
<BOS><User>Huawei is<Assistant></think>
```

它不满足“不要拼接或处理 prompt”的约束，因此不作为本报告的有效结果。该轮
输出恰好为 `Huawei is`，也证明 chat framing 会实质改变输出，不能与纯文本
运行混用。

## 3. 流式输出

| Step | Token ID | Piece | Runner-up | Margin |
| ---: | ---: | --- | ---: | ---: |
| 0 | 260 | ` a` | 270 | 1.287 |
| 1 | 6646 | ` leading` | 7891 | 2.759 |
| 2 | 5217 | ` global` | 16171 | 2.652 |
| 3 | 1951 | ` information` | 7352 | 3.416 |

拼接结果：

```text
Huawei is a leading global information
```

表中的 `decode_output` 只记录新生成部分，所以 summary 中显示的是：

```text
decode_output: token_ids=[260, 6646, 5217, 1951]
decode_output: token_pieces=" a leading global information"
```

4 steps 在 `information` 后停止，句子尚未完成；这不是 EOS，也不能据此评价
完整回答质量。

## 4. 执行路径证据

### 4.1 43 层在 8 节点上的切分

| Guest | Runtime node | Layer range | 每轮记录数 |
| --- | ---: | --- | ---: |
| nodeA | 0 | `[0,6)` | 4 |
| nodeB | 1 | `[6,12)` | 4 |
| nodeC | 2 | `[12,18)` | 4 |
| nodeD | 3 | `[18,23)` | 4 |
| nodeE | 4 | `[23,28)` | 4 |
| nodeF | 5 | `[28,33)` | 4 |
| nodeG | 6 | `[33,38)` | 4 |
| nodeH | 7 | `[38,43)` | 4 |

8 个 QEMU 日志各有 4 条
`deepseek-v4-flash-real-range-runtime: engine=simpler ... status=ok`，合计
32 条。step 0 在每个节点记录 `history_tokens=4 executed_tokens=4`；step 1 到
step 3 分别记录：

```text
step=1 history_tokens=5 executed_tokens=1 position=4
step=2 history_tokens=6 executed_tokens=1 position=5
step=3 history_tokens=7 executed_tokens=1 position=6
```

这证明 step 0 执行 4-token prefill，后续每轮只执行一个新 token，并且 decode
position 连续增长。

nodeH 是 terminal owner。其 4 次真实 range 依次产生 token
`260, 6646, 5217, 1951`，与 stream summary 完全一致。

### 4.2 Handoff 与数据面

每轮有 7 条跨节点边，4 轮共 28 条。summary 给出：

```text
memory_boundary_observation_summary: records=28 steps=4/4
nodes=node1,node2,node3,node4,node5,node6,node7
targets=node2,node3,node4,node5,node6,node7,node8
source=w5_guest_range_exit hidden_backend=obmm_shmem
```

step 0 的 hidden payload 为 262,144 bytes，后续单-token steps 为 65,536
bytes。所有边均记录 `metadata=lingqu_object_service`、`queue=obmm_spsc` 和
`status=ok`。

guest 侧还记录了 `qwen3_range_forward_runtime_input_ub_ssd_gsva_read`。例如
nodeH 在 step 0 到 step 3 分别从 nodeG 对应的 GSVA backend 地址读取输入，
checksum 与生产者发布记录一致。这说明本轮 range handoff 的对象主 backing 是
OBMM shared memory，同时存在并使用并列的 UB-SSD GSVA backend 映射。

### 4.3 连续 decode KV

step 0 在每个节点发布本地 layer KV。step 1、2、3 分别恢复上一轮 KV：

- 每节点 3 条 `deepseek_v4_flash_layer_kv_restored ... status=ok`；
- 8 个节点共 24 条，数量为 `8 * (4 - 1)`；
- restore 的 `previous_step` 依次为 0、1、2；
- KV checksum 与相应上一轮 publish 记录一致。

以 nodeH 为例，KV bytes 随 history 增长：

```text
step=1 previous_step=0 kv_bytes=1343408
step=2 previous_step=1 kv_bytes=1353648
step=3 previous_step=2 kv_bytes=1363888
```

这证明后续 decode 使用了各节点上一轮产生的真实 KV 状态，不是从 step 0
重新 prefill，也不是丢弃 KV 后静默重算。

### 4.4 Decode-round barrier

step 0、1、2 结束后，每个节点各记录一次
`decode_round_barrier ... ready_mask=0xff ... status=ok`，共 24 条。最终 step 3
结束后直接退出，不再等待下一轮 barrier。

该证据证明所有 8 个节点完成当前 round 后才进入下一 round，没有早到节点在
terminal token 提交前提前消费下一步。

## 5. 耗时

| Step | Round time | 说明 |
| ---: | ---: | --- |
| 0 | 1,558,719 ms / 25m 58.719s | 4-token prefill |
| 1 | 392,950 ms / 6m 32.950s | 单-token decode |
| 2 | 379,143 ms / 6m 19.143s | 单-token decode |
| 3 | 375,145 ms / 6m 15.145s | 单-token decode |
| 合计 | 2,705,957 ms / 45m 05.957s | 4 rounds |

后 3 个单-token round 平均为 382,412.7 ms，即约 6m 22.413s/token。
step 0 是最慢 round，占总 round 时间约 57.6%。

这里的 round 时间包含串行的 8-node pipeline 等待。最大的 step 0 input wait
出现在 nodeH，为 1,365,120 ms；最大的单节点 compute window 为 222,046 ms。
因此当前时延主要来自同步 CPU simpler 模型计算及串行 layer pipeline，不是
OBMM/GSVA 边上传输：step 0 的 7 条边总 mono gap 仅 129 ms，后续三轮分别为
326、434、415 ms。

## 6. Cache 与 GSVA 计数解释

本轮 summary 中：

```text
shortpath: lookup_hits=0
prefix_cache: kv_hits=0
gsva: kv_refs=0 reads=0 writebacks=0
```

这些字段统计的是 serving prefix-cache/shortpath reuse，没有命中是预期结果。
它们不代表 W5 range/KV 数据面没有使用 GSVA。guest 详细日志中的
`ub_ssd_gsva_backend_attach`、`ub_ssd_gsva_read` 和 KV restore 是本轮 intra-run
数据面证据。两类计数面向不同生命周期，不能相互替代。

## 7. 资源与异常检查

- OBMM pool：8/8 节点被观察到；
- 每节点 region：512 MiB；集群合计 4 GiB；
- 最大 payload high-water：16,777,280 bytes；
- 最大 arena used：15,728,704 bytes；
- payload 使用率：3.125%；
- 日志目录大小：约 199.2 MiB；
- 未发现 `FAIL:`、panic、error status、fatal marker 或实际 RCU stall；
- 运行结束后 `pgrep -fl qemu-system-aarch64` 无输出，无残留 QEMU 进程。

## 8. 证据位置

- 汇总：
  `guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-07-13_12-59-26_w5_deepseek_v4_flash_decode_6277.txt`
- 8-node 日志目录：
  `guest-linux/aarch64/logs/2026-07-13_12-59-26_w5_deepseek_v4_flash_decode_6277_headless8/`
- nodeH guest stream/KV/GSVA 证据：`nodeH_guest.log`
- nodeA 与 nodeH simpler range 证据：`nodeA_qemu.log`、`nodeH_qemu.log`

## 9. 最终判断

本次运行证明：对于不带任何 chat framing 的纯文本 `Huawei is`，W5 能在
8-node QEMU 集群中通过 sim-uapi 和 simpler C API 执行 DeepSeek V4 Flash
43-layer MoE 路径，完成 prefill、跨节点 hidden handoff、逐节点 KV
publish/restore、decode-round barrier 和 4-token streaming output。

本次没有证明 prefix-cache reuse 收益，也没有证明输出到 EOS。当前最明显的
性能瓶颈仍是同步 CPU simpler 计算和串行 pipeline；UB 数据面边间隔相对模型
计算时间很小。
