# 三节点 DGX Spark Distributed MTP 性能验证报告

日期：2026-07-24

状态：三节点 MTP correctness 已通过；统一入口聚合吞吐达到 20 token/s；
单请求 20 token/s 未达到

## 1. 结论

三节点按层切分的 DeepSeek V4 Flash stream infer 已经支持 distributed MTP，
并且可以稳定执行 N=2 batched target verification。实现保持 greedy 输出精确一致，
没有用错误 token 换速度。

最终实测结论分为两层：

1. **单请求延迟目标未达到。**
   - Q4 + batched MTP N=2：TPOT 约 `93.7 ms`，约 `10.7 token/s`；
   - Q2 no MTP：TPOT 中位数 `75.217 ms`，约 `13.29 token/s`；
   - Q2 + batched MTP N=2：TPOT 中位数 `78.962 ms`，
     约 `12.66 token/s`；
   - 在当前实现上，MTP 使 Q2 单流 TPOT 回归约 `4.98%`；
   - 当前三段串行 pipeline 无法把单流 TPOT 压到 `50 ms`。
2. **统一入口的服务聚合吞吐目标已达到。**
   - 在相同三台 DGX Spark 上运行两个完整的三节点 Q2 + MTP 副本；
   - `0.0.0.0:8000` 的 SSE proxy 使用 least-connections 分流；
   - 两个客户端都访问同一个 `:8000`，各执行 `3 × 256 token`；
   - 合计生成 `1536 token`，并发墙钟时间约 `69.78 s`；
   - 端到端聚合吞吐为 **`22.01 token/s`**；
   - 三组配对请求分别达到 `22.11`、`21.82`、`22.15 token/s`。

因此，“20 token/s”必须明确指标口径：

| 目标口径 | 当前结果 | 判断 |
| --- | ---: | --- |
| 单请求 decode throughput，Q2 no MTP | 13.29 token/s | 未达到 |
| 单请求 decode throughput，Q2 + MTP | 12.66 token/s | 未达到 |
| 双请求服务聚合吞吐 | 22.01 token/s | 已达到 |
| 单一外部 endpoint 自动负载均衡 | `0.0.0.0:8000` | 已达到 |

双副本不是免费收益。并发时每个请求的 TPOT 从单副本 `78.962 ms` 增至约
`90.7 ms`，但三台机器原本因 pipeline 串行产生的空闲区间被另一个副本利用，
所以总吞吐超过 20 token/s。

当前适合定义为：

> 三节点 distributed MTP 已完成 correctness 和性能边界验证。单流优化仍受
> pipeline 串行和 verifier 成本限制；统一入口下的双副本可以提供超过
> 20 token/s 的聚合容量。持久化拉起、全局并发上限和内存水位保护仍需在生产化
> 阶段补齐。

## 2. 验证对象

### 2.1 单副本拓扑

| 节点 | 层范围 | 角色 |
| --- | --- | --- |
| dgx1 | `0:14` | coordinator、HTTP server |
| dgx2 | `15:29` | distributed worker |
| dgx3 | `30:output` | terminal worker、output head、MTP |

每个副本都跨越三台机器。双副本验证不是把三台机器拆成两组，而是在每台机器上
各运行两个相互独立的进程：

```text
Replica A:
  dgx1: coordinator :12340, HTTP 127.0.0.1:8100
  dgx2: worker      :12341
  dgx3: worker      :12342

Replica B:
  dgx1: coordinator :12350, HTTP 127.0.0.1:8101
  dgx2: worker      :12351
  dgx3: worker      :12352

Front door:
  dgx1: proxy 0.0.0.0:8000
```

两个模型 backend 都只监听 loopback。外部唯一入口为
`0.0.0.0:8000`，代理使用 least-connections 选择 backend。

### 2.2 模型

Q4 base：

```text
gguf/DeepSeek-V4-Flash-Q4KExperts-from-NVFP4-imatrix.gguf
size = 164,633,502,560 bytes
```

Q2 base：

```text
gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf
size = 86,720,111,488 bytes
```

MTP：

```text
gguf/DeepSeek-V4-Flash-MTP-Q4K-Q8_0-F32.gguf
size = 3,807,602,400 bytes
```

Q2 和 Q4 的输出质量没有在本轮做系统评测。Q2 的性能数据不能被解释为
“同质量下的无损优化”；它代表一条明确的质量/容量取舍路径。

### 2.3 代码状态

distributed MTP prototype 位于：

```text
out/ds4-distributed-mtp
```

本机 prototype 基于 `4e0b072`。三台 DGX 上的运行目录均为：

```text
/home/dgx/repo/ds4
```

核心修改涉及：

```text
ds4.c
ds4.h
ds4_distributed.c
ds4_distributed.h
ds4_server.c
```

修改仍未提交到 ds4 仓库。原始 `~/repos/ds4` 没有被修改。

## 3. N=2 batched verifier 的实现

旧版 distributed MTP 只生成一个 candidate，然后逐 token 运行完整 target
forward。即使 candidate 全部命中，也没有减少 target pass，因此 Q4 TPOT 从
`87.031 ms` 回归到 `90.989 ms`。

当前 prototype 已改为：

1. 普通 target pass 提交当前 token；
2. dgx3 从当前 hidden-state frontier 递归生成两个 MTP draft；
3. 当前 target logits 免费验证 `draft[0]`；
4. 如果 `draft[0]` 命中，三节点一次处理
   `[draft[0], draft[1]]` 两个 token；
5. dgx3 一次返回两个位置的 target logits；
6. 如果第二个 draft 命中，提交两个 draft；否则只提交第一个；
7. 每个节点保留 speculative prefix frontier，部分命中时回退到 prefix-1；
8. worker 在下一个 WORK 到达时完成 lazy partial commit，不增加控制往返。

这条路径保持 greedy exactness：

```text
accepted token = argmax(target logits)
```

MTP 只负责提出候选，不改变 target model 的最终决定。

## 4. 测量方法

### 4.1 请求配置

| 参数 | 值 |
| --- | --- |
| prompt | `详细解释斐波那切数列的应用场景` |
| prompt token 数 | server 日志为 14 |
| sampling | `temperature=0` |
| thinking | false |
| 单副本正式输出 | 每次 256 token |
| 单副本正式次数 | 3 |
| 单副本预热 | 1 |
| 双副本正式输出 | 每副本 `3 × 256 token` |
| client | dgx1 loopback |

客户端通过 SSE content event 计时：

```text
TTFT = first_content_event_time - request_start_time

TPOT = (last_content_event_time - first_content_event_time)
       / (content_event_count - 1)

E2E = stream_finished_time - request_start_time
```

双副本聚合吞吐使用完整并发批次的墙钟时间：

```text
aggregate throughput
  = total output events / concurrent wall time
  = 1536 / 69.78
  = 22.01 token/s
```

该口径包含 TTFT、decode 和三个请求之间的客户端调度开销，比只计算
`2 / median(TPOT)` 更保守。

### 4.2 网络边界

本轮没有修改任何主机网络、路由、代理或 RoCE 配置。

执行正式请求时，操作机到 `192.168.8.7:8000` 暂时不可达，但 SSH 正常。
因此客户端脚本复制到 dgx1，并通过 `127.0.0.1` 访问 HTTP 服务。模型计算和
dgx1 → dgx2 → dgx3 的三节点数据路径没有变化，只移除了管理机到 dgx1 的
HTTP 接入链路。

## 5. 单请求性能

### 5.1 历史 Q4 对照

| 配置 | TTFT | TPOT | 单流速度 | 说明 |
| --- | ---: | ---: | ---: | --- |
| Q4 no MTP | 425.314 ms | 87.031 ms | 11.49 token/s | 3-run 中位数 |
| Q4 sequential MTP | 619.789 ms | 90.989 ms | 10.99 token/s | 6-run 中位数 |
| Q4 batched MTP N=2 | 1223.568 ms | 93.687 ms | 10.67 token/s | 64-token canary |

Q4 batched N=2 的 TTFT 来自一次冷 canary，不能与有预热的历史中位数做严格对比。
其 TPOT 与另一次独立 canary 的 `93.874 ms` 一致，说明 N=2 路径虽然正确，
仍没有获得净性能收益。

服务端日志中，Q4 N=2 的 64-token decode 约为 `6.150 s`，摊销速度约
`10.41 token/s`。客户端 TPOT 不包含首 token，因此略高。

### 5.2 Q2 + MTP N=2

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 480.050 | 79.044 | 20636.341 | 256 |
| 2 | 480.923 | 77.910 | 20348.031 | 256 |
| 3 | 476.058 | 78.962 | 20611.505 | 256 |
| 中位数 | **480.050** | **78.962** | **20611.505** | 256 |

换算结果：

```text
single-stream throughput = 1000 / 78.962 = 12.66 token/s
```

Q2 相比 Q4 batched N=2 的客户端 TPOT 改善约 `15.7%`，但距离
`50 ms/token = 20 token/s` 仍有明显差距。

### 5.3 Q2 no-MTP 基线

为了区分“Q2 量化收益”和“MTP 收益”，在相同三节点层切分、相同 prompt、
相同 256-token 输出下关闭 MTP，得到：

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 337.588 | 75.089 | 19485.476 | 256 |
| 2 | 337.447 | 75.217 | 19517.761 | 256 |
| 3 | 346.971 | 75.867 | 19693.096 | 256 |
| 中位数 | **337.588** | **75.217** | **19517.761** | 256 |

换算结果：

```text
Q2 no-MTP throughput = 1000 / 75.217 = 13.29 token/s
```

与 Q2 + MTP N=2 对比：

```text
MTP TPOT overhead
  = 78.962 - 75.217
  = 3.745 ms/token
  = 4.98%
```

这说明当前 MTP 的额外 draft 和 verifier 计算没有被接受 token 数摊平。
对单个延迟敏感请求，当前最优配置是 Q2 no MTP，而不是 Q2 + MTP。

## 6. 为什么 N=2 仍然不够快

### 6.1 三段计算仍然串行

一次普通 decode 依次经过：

```text
dgx1 layers 0:14
  → dgx2 layers 15:29
  → dgx3 layers 30:output + MTP
  → logits 返回 dgx1
```

Q4 profile 的典型节点数据为：

| 路径 | dgx2 eval | dgx2 downstream wait | dgx3 eval |
| --- | ---: | ---: | ---: |
| 单 token | 25--26 ms | 35--36 ms | 33--34 ms |
| N=2 verify | 40--43 ms | 41--43 ms | 38--40 ms |

输入 hidden-state payload 约为：

| 请求 | payload |
| --- | ---: |
| 单 token | 0.06 MiB |
| N=2 | 0.13 MiB |

返回 logits payload 约为：

| 请求 | payload |
| --- | ---: |
| 单 token | 0.49 MiB |
| N=2 | 0.99 MiB |

payload 很小，且节点 compute/wait 已占主要时间。200 Gb rail 可以减少通信尾部，
但不能把当前约 `79--94 ms` 的单流 TPOT直接降到 `50 ms`。

### 6.2 Q2 no-MTP 三节点分解

在三台机器同时启用 distributed decode profiler，64 个单-token 样本的均值为：

| 节点 | 本地计算 | downstream/remote | 总计 |
| --- | ---: | ---: | ---: |
| dgx1 coordinator | 20.915 ms | 53.986 ms | 74.903 ms |
| dgx2 worker | 21.257 ms | 约 32.6 ms 等待 dgx3 | 约 53.9 ms |
| dgx3 terminal | 20.778 ms | 2.696 ms 发送 logits | 23.481 ms |

dgx1 的 remote 部分进一步分解为：

```text
send activation:       0.039 ms
wait downstream:      53.839 ms
copy returned logits:  0.016 ms
payload:                0.49 MiB
```

三段 GPU 计算都约为 `21 ms`，说明 `15 / 15 / 13 + output + MTP` 的层切分
已经基本平衡。继续挪一两层只能移动等待位置，不能减少三个阶段的串行和。
网络及协议残差约 `9--12 ms`，即使把它完全消除，仍无法达到 `50 ms`。

### 6.3 CUDA decode stage profile

对 dgx1 layer 0、dgx2 layer 15、dgx3 layer 30 分别做逐 stage CUDA
同步计时。代表性单层均值如下：

| Stage | dgx1 L0 | dgx2 L15 | dgx3 L30 |
| --- | ---: | ---: | ---: |
| routed MoE | 0.370 ms | 0.368 ms | 0.370 ms |
| attention output | 0.333 ms | 0.338 ms | 0.347 ms |
| Q path | 0.242 ms | 0.224 ms | 0.227 ms |
| compressor/indexer | 0.002 ms | 0.085 ms | 0.205 ms |
| attention | 0.046 ms | 0.046 ms | 0.074 ms |
| shared gate/up | 0.097 ms | 0.096 ms | 0.099 ms |
| HC pre，attention + FFN | 0.159 ms | 0.166 ms | 0.175 ms |
| 其余 stage 合计 | 0.134 ms | 0.142 ms | 0.166 ms |
| 单层合计 | **1.383 ms** | **1.465 ms** | **1.663 ms** |

普通单-token MoE 的内部 kernel 分解为：

| 节点 | gate/up | down | MoE total |
| --- | ---: | ---: | ---: |
| dgx1 | 0.232 ms | 0.113 ms | 0.356 ms/layer |
| dgx2 | 0.234 ms | 0.116 ms | 0.359 ms/layer |
| dgx3 | 0.263 ms | 0.127 ms | 0.402 ms/layer |

N=2 verifier 的 MoE 为 `0.765--0.820 ms/layer`。当前实现已经默认使用
decode LUT gate 和 6-expert direct-down-sum kernel。即使把整个 routed MoE
成本假设为零，三节点最多节省约 `15.5 ms/token`，仍不足以把
`75--79 ms` 降到 `50 ms`。达到单流 20 token/s 需要同时优化 MoE、
attention output、Q projection 和跨节点调度，而不是再切换一个 MoE 开关。

### 6.4 已排除的 activation 传输量化

在 Q2 no-MTP 基线上测试 coordinator 的 activation wire format：

| activation bits | TPOT 中位数 | 相对 32-bit | 输出 |
| ---: | ---: | ---: | --- |
| 32 | **75.217 ms** | baseline | 基准 hash |
| 16 | 75.809 ms | 慢 0.79% | hash 改变 |
| 8 | 75.939 ms | 慢 0.96% | hash 改变 |

activation payload 虽然减小，但量化/反量化成本抵消了传输收益，并改变 greedy
输出。该路径既没有性能收益，也不满足本轮 exactness 要求，已经排除。

### 6.5 GPU 频率边界

64-token 生成期间，三台 GB10 的 SM 频率约为 `2.42--2.45 GHz`，而
`nvidia-smi` 报告的硬件最大值为 `3.003 GHz`。同时：

- 温度仅 `47--49°C`；
- GPU 瞬时功耗约 `18--20 W`；
- 没有 thermal slowdown 或 HW power brake；
- 默认 application clock 为 `2.418 GHz`；
- 历史计数显示存在 software power capping。

这表明当前平台策略限制了可用频率，而不是散热限制。锁高频属于三台机器的
系统级功耗策略变更，本轮没有擅自修改。即使按频率比做过于乐观的线性外推，
`75.217 × 2.418 / 3.003 ≈ 60.6 ms`，仍只有约 `16.5 token/s`；
它值得单独做经授权的可逆实验，但不能单独保证 20 token/s。

### 6.6 verifier 节省的 pass 被 MTP 和 miss 抵消

Q4 N=2 canary 的 22 个实际 verifier cycle 中：

- 14 次提交两个 draft；
- 8 次只提交一个 draft；
- 条件第二 draft 命中率为 `14 / 22 = 63.6%`；
- N=2 verifier 内的 draft-token 接受率为 `36 / 44 = 81.8%`。

但还有 6 个 cycle 在第一个 draft 就 miss，不会进入 verifier。按完整 64-token
生成过程计算：

- 总 speculative cycle：28；
- 总 draft proposal：56；
- 总接受 draft：36；
- 无条件 draft-token 接受率：`64.3%`。

Q4 N=2 的平均时间：

```text
normal target + MTP suffix: 96.6 ms
N=2 target verify:          156.3 ms
```

即使某个 cycle 提交三个输出 token，`96.6 + 156.3 = 252.9 ms`，
理想摊销仍约为 `84.3 ms/token`。发生 partial accept 或 first-draft miss 后，
收益进一步消失。

## 7. 深草稿验收率

为了判断 N=4/8 是否值得实现，运行了一个 `--mtp-draft 8` 探针：

- 关闭 speculative commit；
- 每个正常 target token 后记录 8 个递归 MTP draft；
- 用后续真实 greedy token 计算连续命中长度；
- 生成 64 个真实 token；
- 尾部不足 8 个未来 token 的窗口按各深度分别剔除。

结果：

| 连续命中深度 | 命中/可评估窗口 | 命中率 |
| ---: | ---: | ---: |
| ≥1 | 54/63 | 85.7% |
| ≥2 | 39/62 | 62.9% |
| ≥3 | 21/61 | 34.4% |
| ≥4 | 8/60 | 13.3% |
| ≥5 | 2/59 | 3.4% |
| ≥6 | 1/58 | 1.7% |
| ≥7 | 0/57 | 0% |
| ≥8 | 0/56 | 0% |

56 个完整 8-token 窗口的连续接受长度分布：

| 接受长度 | 窗口数 |
| ---: | ---: |
| 0 | 9 |
| 1 | 11 |
| 2 | 16 |
| 3 | 12 |
| 4 | 6 |
| 5 | 1 |
| 6 | 1 |

平均连续接受长度只有 **2.04 token**。因此把当前 generic batched verifier
机械扩展到 N=8 会计算大量最终无法提交的行，不能解决单流 20 token/s。

该探针的 TPOT 为 `122.574 ms`，但它在每个真实 token 后都额外生成 8 个 draft，
只用于验收率测量，不是候选部署配置。

## 8. 双副本聚合吞吐

### 8.1 逐副本数据

两个副本同时运行。每个副本连续执行三次 256-token 请求。

Replica A：

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 728.449 | 87.591 | 23064.166 | 256 |
| 2 | 542.021 | 90.756 | 23684.851 | 256 |
| 3 | 535.417 | 90.913 | 23718.299 | 256 |
| 中位数 | 542.021 | **90.756** | 23684.851 | 256 |

Replica B：

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 749.422 | 87.755 | 23127.082 | 256 |
| 2 | 564.355 | 90.664 | 23683.811 | 256 |
| 3 | 549.532 | 90.862 | 23719.488 | 256 |
| 中位数 | 564.355 | **90.664** | 23683.811 | 256 |

两个副本的结果高度对称，说明没有一个副本长期饿死，也不是单个快请求制造的
吞吐假象。

### 8.2 配对聚合吞吐

每组配对吞吐使用两个同序号请求中较慢者的 E2E：

| Pair | 合计 token | 配对墙钟 (s) | 聚合吞吐 |
| ---: | ---: | ---: | ---: |
| 1 | 512 | 23.127 | 22.14 token/s |
| 2 | 512 | 23.685 | 21.62 token/s |
| 3 | 512 | 23.719 | 21.59 token/s |

完整并发批次：

| 指标 | 值 |
| --- | ---: |
| 总输出 | 1536 token |
| Replica A 进程墙钟 | 71.68 s |
| Replica B 进程墙钟 | 71.75 s |
| 保守并发墙钟 | 71.75 s |
| 聚合吞吐 | **21.41 token/s** |

相比单副本，两个请求各自约慢 `15%`，但总容量从 `12.66 token/s` 增长到
`21.41 token/s`，扩展效率约为：

```text
21.41 / (2 × 12.66) = 84.6%
```

这符合 pipeline parallel 的预期：副本 A 在 dgx2/dgx3 计算时，副本 B 可以使用
dgx1；反之亦然。两个副本同时争用同一节点时会产生 CUDA 和内存带宽竞争，所以
没有达到理想的 2 倍。

### 8.3 统一入口验收

增加 `dgx_ds4_proxy.py` 后，两个客户端均请求
`http://127.0.0.1:8000`。代理日志证明每一对请求分别进入 `8100` 和 `8101`，
没有由客户端直接选择 backend。

Client 1：

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 834.625 | 87.525 | 23153.686 | 256 |
| 2 | 610.194 | 89.625 | 23464.619 | 256 |
| 3 | 572.993 | 88.357 | 23104.121 | 256 |
| 中位数 | 610.194 | **88.357** | 23153.686 | 256 |

Client 2：

| Run | TTFT (ms) | TPOT (ms) | E2E (ms) | Events |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 814.822 | 87.465 | 23118.475 | 256 |
| 2 | 592.894 | 89.558 | 23430.209 | 256 |
| 3 | 551.760 | 88.493 | 23117.580 | 256 |
| 中位数 | 592.894 | **88.493** | 23118.475 | 256 |

配对和完整批次：

| Pair | 合计 token | 配对墙钟 (s) | 聚合吞吐 |
| ---: | ---: | ---: | ---: |
| 1 | 512 | 23.154 | 22.11 token/s |
| 2 | 512 | 23.465 | 21.82 token/s |
| 3 | 512 | 23.118 | 22.15 token/s |
| 全部 | 1536 | 69.78 | **22.01 token/s** |

6 个正式请求均返回 256 个 content event，输出文本 SHA-256 只有一个唯一值，
证明两个 backend 和三个重复 run 的 greedy 输出一致。

代理没有形成性能瓶颈。统一入口结果比直接访问两个 backend 的首次结果还略高，
差异主要来自复测时 CUDA/model cache 已充分预热，不能解释为代理带来了计算收益。

## 9. 资源占用与部署风险

正式双副本基准完成后：

| 节点 | 系统内存 used | available | ds4 CUDA 进程 |
| --- | ---: | ---: | ---: |
| dgx1 | 89 GiB | 32 GiB | 2 × 40,417 MiB |
| dgx2 | 94 GiB | 26 GiB | 2 × 39,393 MiB |
| dgx3 | 112 GiB | 8.7 GiB | 2 × 40,055 MiB |

三台机器均无 swap。dgx3 的可用内存只有约 `8.7 GiB`，是当前双副本部署的主要
稳定性风险。长 context、更多并发 session、文件缓存变化或其他 GPU workload
都可能触发内存压力。

当前不能继续增加第三个副本。生产化前至少需要：

1. 设置进程级和系统级内存水位监控；
2. 达到低水位时拒绝新请求，而不是等待 OOM；
3. 禁止在同一节点并存 vLLM、ComfyUI 等大内存 workload；
4. 做 4K、长 context 和持续并发 soak；
5. 验证进程异常退出后的 lock 和端口回收。

每个副本通过独立 `DS4_LOCK_FILE` 隔离 instance lock。Replica B 使用：

```text
DS4_LOCK_FILE=/tmp/ds4-replica-b.lock
```

## 10. 对用户的影响

### 单请求用户

Q2 no-MTP 是当前最佳单请求延迟配置：

- TTFT 中位数约 `338 ms`；
- steady-state TPOT 约 `75.22 ms`；
- 单流速度约 `13.29 token/s`。

如果必须启用 MTP：

- TTFT 中位数约 `480 ms`；
- steady-state TPOT 约 `78.96 ms`；
- 单流速度约 `12.66 token/s`；
- 当前实现对单流是约 5% 的性能回归。

双副本满载时：

- 每个请求 TPOT 约 `90.7 ms`；
- 单请求速度约 `11.0 token/s`；
- 用户会感受到约 15% 的 decode 变慢。

### 多用户服务

双副本允许两个长请求真正并行：

- 单副本只能串行处理 server job queue；
- 双副本可把两个请求分发到不同三节点 pipeline；
- 统一入口聚合吞吐达到 22.01 token/s；
- 第二个请求不再等待第一个完整生成结束。

所以它优化的是排队时间和服务容量，不是单个用户的 token 间隔。

## 11. 当前运行状态

报告完成时，两套 Q2 + MTP N=2 副本仍在运行：

```text
Replica A:
  HTTP 127.0.0.1:8100
  distributed ports 12340/12341/12342

Replica B:
  HTTP 127.0.0.1:8101
  distributed ports 12350/12351/12352

Front door:
  HTTP 0.0.0.0:8000
  least-connections → 8100/8101
```

代理由以下 CLI 提供：

```text
python3 dgx_ds4_proxy.py \
  --listen-host 0.0.0.0 \
  --listen-port 8000 \
  --backend http://127.0.0.1:8100 \
  --backend http://127.0.0.1:8101
```

`http://192.168.8.7:8000/healthz` 已从 dgx2 验证，返回两个健康 backend。
代理支持 SSE 无缓冲转发、least-connections、断连传播和周期健康检查。

当前 proxy 健康检查调用 backend 的 `/v1/models`，只证明 HTTP 进程存活，
不能证明 distributed route 已经包含 dgx2 和 dgx3。报告完成前另外执行了真实
streaming generation，确认两条三节点链路可推理。生产化时应把 backend
health 改为 route-aware readiness，避免 coordinator 存活但 worker 未就绪时
出现假健康。

## 12. 验证和数据制品

原始数据位于：

```text
out/dgx-three-node-mtp-tpot-20260724/
├── ds4-tpot-A.json
├── ds4-tpot-B1.json
├── ds4-tpot-B2.json
├── q4-mtp-batched-n2-v3.json
├── q4-mtp-batched-n2-v4.json
├── q4-mtp-batched-n2-profile.json
├── deep-draft-probe.json
├── deep-draft-probe-dgx1.log
├── q2-mtp-n2-benchmark.json
├── q2-mtp-n2-dgx1.log
├── q2-no-mtp-profile.json
├── q2-base-profile.json
├── q2-base-profile-dgx1.log
├── q2-base-profile-dgx2.log
├── q2-base-profile-dgx3.log
├── q2-act16.json
├── q2-act8.json
├── cuda-moe-profile-dgx1.log
├── cuda-moe-profile-dgx2.log
├── cuda-moe-profile-dgx3.log
├── decode-stage-profile-dgx1.log
├── decode-stage-profile-dgx2.log
├── decode-stage-profile-dgx3.log
├── q2-mtp-dual-a.json
├── q2-mtp-dual-b.json
├── q2-mtp-dual-a-dgx1.log
├── q2-mtp-dual-b-dgx1.log
├── unified-client-1.json
├── unified-client-2.json
├── unified-proxy.log
├── unified-backend-a-dgx1.log
└── unified-backend-b-dgx1.log
```

本轮代码验证：

- 本地 `make cpu`：通过；
- 本地 `make ds4_test && ./ds4_test --server`：通过；
- 本地 `git diff --check`：通过；
- `cargo test --workspace`：通过；
- `python3 -m unittest discover guest-linux/aarch64/tests`：
  328 个测试通过，1 个跳过；
- `python3 -m unittest guest-linux/aarch64/tests/test_dgx_ds4_proxy.py`：
  6 个测试通过；
- dgx1、dgx2、dgx3 `make cuda-spark`：通过；
- 三节点 Q4 N=2 64-token canary：通过；
- 三节点 Q2 N=2 `1 warmup + 3 × 256 token`：通过；
- 双副本直连并发 `2 × 3 × 256 token`：通过；
- 统一入口并发 `2 × 3 × 256 token`：通过，22.01 token/s。

`make test` 的完整模型测试需要本地默认 `ds4flash.gguf`，当前操作机缺少该制品。
已执行的子测试通过，但不能宣称 ds4 的全量模型测试通过。

## 13. 最终判断与后续顺序

| 目标 | 结果 |
| --- | --- |
| 三节点 Q4 stream infer | 通过 |
| dgx3 加载并执行 MTP | 通过 |
| distributed N=2 batched verifier | 通过 |
| greedy 输出精确提交 | 通过 |
| Q4 单流降低 TPOT | 未通过 |
| Q2 no-MTP 单流达到 20 token/s | 未通过，13.29 token/s |
| Q2 单流达到 20 token/s | 未通过，12.66 token/s |
| 双副本聚合达到 20 token/s | **通过，22.01 token/s** |
| 单入口提供 20 token/s | **通过，22.01 token/s** |

后续顺序：

1. 将两个 coordinator 和 proxy 纳入持久化进程管理；
2. 把 backend 健康检查升级为 distributed route-aware readiness；
3. 加入全局并发上限、背压和内存低水位拒绝；
4. 做 4K 和长 context 双并发 soak；
5. 如果目标仍是单流 20 token/s，按优先级评估：
   经授权的 GB10 高频模式、原生 NVFP4 CUDA 路径、跨 session/微批次 pipeline
   scheduler；不要继续机械扩大 MTP draft depth。
