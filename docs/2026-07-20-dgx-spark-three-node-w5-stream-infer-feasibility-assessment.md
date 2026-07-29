# 三节点 DGX Spark W5 Stream Infer 部署可行性与差距评估

日期：2026-07-20

状态：评估完成；Node A 的 ds4 Q2/GB10 server startup 已实机通过

## 1. 结论

在三台 DGX Spark 上提供 DeepSeek V4 Flash 三节点流式推理服务可行，而且不需要从零
开发 CUDA inference engine 或 serving API。`ds4` 已经提供 DGX Spark/GB10 CUDA
backend、按层切分的多节点推理、worker-to-worker TCP pipeline、OpenAI/Anthropic API、
SSE、KV snapshot 和单机 DGX 实测结果。

当前已经不只是源码可行性：Node A 使用 `ds4@80ebbc3` 和 Q2 imatrix GGUF，成功完成
GB10 `sm_121` CUDA 初始化、80.24 GiB model mapping、100k context buffer 分配、disk KV
cache 初始化和 HTTP listen。单节点 runtime/build/model compatibility 门禁已经通过；实际
API 生成、两节点和三节点 distributed 仍需留下可复现 evidence。

准确判断是：

> 最直接路径是以 `ds4` 作为 DGX native GPU execution plane 和 serving plane，
> 以 W5 作为官方 checkpoint、模型语义和分布式状态机 oracle，以 mem_service
> 作为每节点本地 metadata/artifact sidecar。当前缺口从“实现 GPU runtime”缩小为
> “验证并产品化 `ds4` 的三节点 CUDA distributed 路径，以及完成 mem_service 集成”。

因此建议先做部署验证，而不是开发新 runtime：

1. 在空闲 DGX Spark 上用同一 `ds4` commit、镜像和 Q2 GGUF 完成单节点 CUDA 回归；
2. 先跑两节点，再按 `0:14`、`15:28`、`29:output` 跑三节点 distributed CLI；
3. 使用现有 TCP data path 绑定一条 200 GbE 直连 rail，完成 1/4-step 和长 prompt；
4. 在 Node A 用现有 `ds4-server` 提供 OpenAI-compatible/SSE 服务；
5. 将 mem_service 作为每节点本地 metadata/artifact sidecar，暂不把它误当成
   已具备一致性的分布式内存服务；
6. 验收后再决定是否将 ds4 TCP transport 升级为多 rail、RDMA/GPUDirect 数据面。

可行性分级：

| 目标 | 判断 | 原因 |
| --- | --- | --- |
| 将当前 W5 命令原样放到三台 DGX 上 | 不可行 | 当前三节点是单机 3 个 QEMU guest 和模拟 UB fabric |
| 用 `ds4` 在三台 DGX 上跑 correctness MVP | 高度可行，待实机验收 | CUDA、layer slice、TCP pipeline 已实现，但无三节点 Spark 通过证据 |
| 提供单请求、短上下文流式服务 | 高度可行，待部署验收 | `ds4-server` 已有 HTTP、OpenAI/Anthropic compatibility 和 SSE |
| 提供多租户生产服务 | 当前不可宣称 | server 单 graph worker、无 batching，distributed protocol 无认证/加密 |
| 加载 W5 官方 Safetensors 原权重 | `ds4` 路径不可行 | `ds4` 只支持项目发布的特定 GGUF，不能直接消费任意官方 checkpoint |
| 宣称 1M context | 当前不可宣称 | 必须分别完成 ds4 三节点长上下文容量、正确性和稳定性验证 |

从用户目标出发，三台机器主要有以下部署方式：

| 用户目标 | 推荐拓扑 | 原因 |
| --- | --- | --- |
| 最低 decode 延迟 | 单节点 Q2；另外两台不进入 token hot path | Q2 约 81 GB，单台 Spark 可容纳；避免每 token 两次跨机 hop |
| 最大总吞吐/基础可用性 | 三台各运行一个 Q2 replica，入口做请求级路由 | 三个独立 graph worker 可并行服务三个请求，单机故障不拖垮全部请求 |
| 更高模型质量、运行 Q4 | 三节点 layer pipeline | Q4 约 153 GB，单台 121 GiB 不适合常驻，三节点按层切分合理 |
| 直接加载官方 Safetensors | W5-native 新执行面 | ds4 不支持该制品，开发和验收量级显著更大 |

本文后续“三节点 pipeline”以 Q4 为目标服务制品；Q2 仅用于最快验证 CUDA 和
distributed correctness。若用户目标实际是低延迟或总吞吐，应改为三副本，而不是强行
让每个 token 经过三台机器。

## 2. 评估范围与证据

本评估基于：

- 当前仓库 `master`，HEAD 为 `4dc9148`；
- `ds4` 仓库 `main`，HEAD 为 `80ebbc3`；
- W5 DeepSeek V4 Flash official checkpoint 计划与现有实现；
- `ds4` 的 DGX Spark CUDA backend、distributed runtime、server、测试和 git 历史；
- Node A 的 ds4 Q2/GB10/100k-context server startup 日志和只读复核；
- 三台机器上现有 NVIDIA DeepSeek V4 Flash NVFP4 checkpoint 的只读格式检查；
- 2026-07-18 的两节点 official checkpoint 1-step 真实通过记录；
- 2026-07-19 至 2026-07-20 的两节点 4-step 真实运行记录；
- 三台 DGX Spark 的只读系统、GPU、容器、网络、RDMA、内存和磁盘探测。

未执行：

- 未停止或修改三台机器上的现有 workload；
- 未安装软件或修改网络配置；
- 未运行 `ib_write_bw`、`nccl-tests` 等可能占用链路/GPU 的性能测试；
- 未复制 checkpoint；
- 未复制 `ds4` GGUF 到另外两台机器；
- 未在 Node B/C 构建或运行 `ds4`；
- 本轮未重新启动服务；复核时 Node A 的 ds4-server 已停止；
- 未发送新的 inference request。

## 3. DGX Spark 环境现状

### 3.1 计算和系统

三台机器的基础环境一致：

| 项目 | 实机结果 |
| --- | --- |
| 机器 | NVIDIA DGX Spark |
| 架构 | arm64 |
| OS | Ubuntu 24.04.4 LTS |
| Kernel | 6.17.0-1014-nvidia |
| GPU | NVIDIA GB10，每台 1 个 |
| Driver | 580.142 |
| Driver CUDA compatibility | 13.0 |
| 统一内存 | 每台约 121 GiB |
| 根盘 | 每台约 3.7 TiB |
| 根盘可用 | 约 2.8--2.9 TiB |
| Docker | 29.2.1 |
| NVIDIA Container Toolkit | 1.17.8 |

宿主机当前没有可用的 `nvcc`。这不阻塞容器化部署，但意味着不能依赖宿主机裸环境
执行 `make cuda-spark`；必须提供统一、可复现、包含 CUDA toolkit、cuBLAS 和 C toolchain
的开发/运行容器。

现有容器内可见：

- Node A/B：PyTorch 2.11.0 + CUDA 13.0 + NCCL 2.28.9；
- Node C：PyTorch 2.12 nightly + CUDA 13.2 + NCCL 2.29.7。

版本不一致，不能作为新服务的正式集群环境。不过 `ds4` CUDA backend 直接使用
CUDA runtime 和 cuBLAS，当前 distributed transport 是 TCP，不依赖 PyTorch 或 NCCL。
因此部署约束应改为相同的 `ds4` commit、镜像 digest、CUDA/cuBLAS toolchain 和 GGUF
checksum；只有未来切换 NCCL/RDMA 数据面时才需要锁定 NCCL 版本。

### 3.2 当前资源占用

最新只读复核结果：

| 节点 | 可用统一内存 | 现状判断 |
| --- | ---: | --- |
| Node A | 约 113 GiB | 当前无 GPU compute process，可以继续单节点 ds4 验证 |
| Node B | 约 4.0 GiB | 既有 vLLM worker 占用约 99.6 GiB，不能启动 ds4 worker |
| Node C | 约 54 GiB | 既有 GPU workload 占用约 32.6 GiB，暂不适合目标 worker |

Node A 已具备继续验证的资源条件；三节点实跑仍必须由环境所有者为 Node B/C 安排维护
窗口，停止或迁移既有 workload。本评估没有执行清理。

### 3.3 RoCE full mesh

每台机器均探测到 4 个 RoCE device：

```text
rocep1s0f0
rocep1s0f1
roceP2p1s0f0
roceP2p1s0f1
```

共同状态：

- RDMA link `ACTIVE`，physical state `LINK_UP`；
- 对应 netdev 全部 `UP`；
- MTU 为 9000；
- 实测端口协商速率为 200000 Mb/s、Full Duplex、RS FEC；
- 三对节点之间各存在两条独立 IPv4 直连子网；
- 所有方向、所有 rail 的低流量 ICMP 探测均为 0% 丢包；
- `ib_write_bw` 已安装在三台机器上。

这证明物理链路、RoCE device 和 L3 双 rail 拓扑具备。`ds4` 当前使用普通 TCP socket，
所以首个 MVP 只需要验证 TCP 绑定到直连 200 GbE netdev 后的吞吐、时延和稳定性；它
不会因为底层网卡支持 RoCE 就自动使用 RDMA 或 GPUDirect。正式生产资格仍必须补：

1. 每对节点、每条 rail 的 `ib_write_bw`/`ib_read_bw`；
2. 双 rail 聚合与单 rail 降级；
3. `iperf3` 单 rail TCP 吞吐、并发流和 tail latency；
4. ds4 hidden/logits 实际 payload 的端到端 profile；
5. PFC/ECN、pause、FEC、MTU 和丢包/重传计数；
6. 并发 workload 下的 tail latency。

`nccl-tests`、GPUDirect RDMA 和 multi-rail 验证是未来升级 NCCL/RDMA transport 的
门禁，不是 ds4 TCP MVP 的前置门禁。ds4 当前一个 hop 使用一个 host/port，不能自动
聚合两条 rail；首版应明确选择一条 rail，另一条 rail 只作为人工切换/故障测试对象。

### 3.4 Node A ds4 单节点实测

已在 Node A、`ds4@80ebbc3` 上使用 Q2 imatrix GGUF 启动：

```text
ds4-server --ctx 100000 --kv-disk-dir <kv-dir> --kv-disk-space-mb 8192
```

实测 evidence：

| 项目 | 结果 |
| --- | --- |
| GPU/backend | NVIDIA GB10 CUDA，`sm_121` |
| GGUF 文件大小 | 86,720,111,488 bytes，约 80.76 GiB |
| CUDA model tensor mapping | 80.24 GiB |
| startup tensor span coverage | 80.76 GiB，11.599 秒 |
| context | 100,000 tokens |
| context buffers | 2,461.24 MiB |
| prefill chunk | 4,096 tokens |
| disk KV budget | 8,192 MiB |
| server | 成功监听 loopback HTTP 端口 |

这证明以下门禁已经通过：

- 当前 ds4 commit 可以在 GB10/sm_121 启动；
- 当前 Q2 GGUF 与 CUDA runtime 兼容；
- 80.24 GiB 权重和 100k context buffer 可以在单台 121 GiB Spark 上完成 admission；
- disk KV cache 和 HTTP server 初始化成功。

仍不能从启动日志推导：

- API 请求已经生成正确 token；
- 100k 实际 prompt 已完成 prefill/decode；
- server 长稳、并发、KV hit/restart 已通过；
- distributed CUDA 已通过。

`CUDA host registration skipped: operation not supported` 没有阻止服务启动，但意味着
不能假设 host buffer 已被 CUDA pin/register。单节点主要影响 staging 性能；三节点 TCP
pipeline 还需要观察 GPU/host copy、socket send 和 tail latency。

进程设置 `oom_score_adj=1000`，在系统内存压力下会优先被 OOM killer 选择。MVP 可以
保留这一 fail-fast 策略，但必须配合 admission、systemd/container restart 和 readiness；
不能与其他大模型 workload 共享到接近 121 GiB 上限。

## 4. 可以复用的现有能力

### 4.1 官方 checkpoint 与模型语义

当前 loader 已经直接支持官方 Safetensors：

- 46 个 shard；
- 69,187 个 tensor；
- checkpoint payload 为 159,609,485,896 bytes，目录约 149 GiB；
- FP8 E4M3、F8 E8M0 scale 和 packed FP4 expert；
- positioned read 和有界 tensor/expert cache；
- 完整 config/index/shard/schema fail-closed gate；
- tokenizer、真实 logits candidate 和 token text metadata。

这些模型资产解析、量化格式、tensor mapping、reference oracle 和 checksum contract
可以复用。它们不应重写成另一套不可对照的模型定义。

### 4.2 三节点 layer pipeline 语义

当前 DeepSeek V4 Flash 有 43 层，三节点均衡划分为：

| 节点 | 当前 W5 layer range | 层数 |
| --- | --- | ---: |
| Node A | `[0,15)` | 15 |
| Node B | `[15,29)` | 14 |
| Node C | `[29,43)` | 14 |

当前状态机已经定义：

```text
prompt/token history
-> Node A range forward
-> hidden handoff
-> Node B range forward
-> hidden handoff
-> Node C range forward + final logits
-> terminal token publish
-> next decode step restores local KV
```

可复用的不是 QEMU transport，而是以下语义：

- layer ownership；
- per-step history/position；
- local range KV publish/restore；
- hidden handoff version/checksum；
- terminal token/logits artifact；
- decode barrier；
- model/session/version/checksum fail-closed 校验。

### 4.3 ds4 DGX native execution 与 serving plane

`ds4` 当前已经提供：

- Linux `make cuda-spark` 构建目标，专门覆盖 DGX Spark/GB10；
- DeepSeek V4 Flash 专用 CUDA backend 和 cuBLAS 路径；
- 项目发布的 DeepSeek V4 Flash GGUF loader，支持 43 层和 layer slice mapping；
- coordinator/worker 分布式执行，支持任意连续 layer range；
- `A -> B -> C -> A` 的 worker-to-worker TCP data path；
- 大 prompt 的 4096-token chunked/pipelined prefill；
- FP32/FP16/INT8 activation wire format；
- rolling token-prefix hash、request ID、worker reconnect、KV replay 和 snapshot；
- OpenAI Chat/Responses/Completions、Anthropic Messages 和 SSE；
- CLI、server、bench、eval 和 agent 共用同一执行面。

`ds4` README 记录的单台 DGX Spark GB10、Q2、7047-token 实测为 prefill
343.81 tokens/s、generation 13.75 tokens/s。这只能证明单节点 GB10 CUDA 路径曾经跑通，
不能替代当前 commit、当前三台机器和三节点 topology 的重新验收。

从源码边界看，distributed worker 使用同一个 `ds4_engine` 创建 session，并调用
`ds4_session_eval_layer_slice()`；Linux CUDA build 把 `ds4_cuda.o` 链入 CLI 和 server。
因此 distributed 和 CUDA 不是两套互斥路径，三节点 CUDA 部署具备实现基础。

同时存在四个明确限制：

1. 公开证据主要是单 DGX CUDA 和双节点 Metal distributed，没有当前 commit 的三节点
   DGX Spark CUDA 通过记录；
2. transport 是带 `TCP_NODELAY` 的普通 TCP，不是 NCCL、RDMA 或 GPUDirect；
3. `ds4-server` inference 由单 graph worker 串行执行，不做 continuous batching；
4. distributed protocol 未加密、未认证且未承诺 wire compatibility，只适合可信内网和
   同一 commit。

### 4.4 mem_service 核心与本地 sidecar

`mem_service` 已具备：

- core-only host binary；
- Unix-socket daemon/client；
- object、prefix、KV、runtime handoff、execution artifact RPC；
- typed C client wrapper；
- snapshot+journal、幂等、审计、metrics；
- systemd/package/release contract；
- local block、chunked block 和 TCP payload fetch 骨架；
- serving example 和安装后 SDK smoke。

这些能力适合在每台 DGX 上作为本地 sidecar，管理本节点的：

- model binding；
- checkpoint revision/checksum；
- KV segment metadata；
- hidden/token artifact metadata；
- request/session audit；
- local durable catalog。

## 5. 纳入 ds4 后仍需解决的差距

### 5.1 W5 本身仍不能直接跨三台物理机

当前 `--nodes 3` 启动的是同一台 host 上的三个 QEMU guest：

```text
host harness
-> QEMU nodeA
-> QEMU nodeB
-> QEMU nodeC
-> simulated UB fabric manager/full-mesh topology
```

它没有跨物理 host 启动、真实网络 endpoint 或跨主机故障处理。把三个 QEMU 分别放到
三台机器也不能自动形成当前模拟 UB fabric。

这不再要求为 W5 新建 production transport：生产部署直接使用 ds4 coordinator/worker
runtime，W5 保留为 correctness oracle 和 contract source。

### 5.2 ub_sim 没有 GPU backend，但 ds4 已补齐执行面

当前正式计算路径是：

```text
W5 guest
-> UAPI dispatch
-> sim-uapi
-> sim-chipbackend-simpler
-> simpler CPU simulator runtime
```

`ub_sim` 中没有 DeepSeek V4 production CUDA backend，`sim-chipbackend-simpler` 动态
加载的仍是 simpler host runtime，不会调用 GB10 Tensor Core。

但 ds4 已有 GB10 CUDA backend、`make cuda-spark` 和 DGX 实测。用户影响从“等待 4--8
周开发 CUDA backend”变成“在当前三台机器上构建、加载指定 GGUF 并完成回归”。除非
业务硬性要求直接加载 W5 官方 Safetensors，否则不应再开发第二套 W5 CUDA engine。

### 5.3 ds4 TCP 可以先跑，但尚未利用 RoCE/RDMA 能力

当前 hidden/KV/object 数据面依赖 guest 内的：

- OBMM pool/SPSC queue；
- GSVA/GVA；
- simulated UB SSD；
- `/sys/bus/ub`、UAPI resource 和 doorbell/CQ；
- QEMU fabric manager。

DGX Spark 的网络不能直接消费这些 descriptor，但 ds4 已定义独立的 host-native
distributed protocol，包含 route、session、request、token hash、shape 和 hidden/logits
payload，可以绕过 W5 模拟设备。

剩余差距是性能和网络工程：ds4 的 socket 连接一次只选择一个 IP/rail，hidden payload
经过 CPU 可见内存和 TCP stack，不具备 GPUDirect，也没有双 rail 聚合。对首个 correctness
和服务 MVP，这比先开发 NCCL/RDMA 更直接；只有实际 profile 证明 TCP 是瓶颈后，才应
升级 transport。

### 5.4 mem_service 不是跨主机热数据面

当前业务 RPC daemon 只监听 Unix socket。TCP 能力主要是 metrics listener 和
`transport-tcp-block-v1` 的 payload fetch/认证路径；它不是支持持续 hidden/KV/token
流量的双向跨主机 RPC，更不是 RDMA transport。

当前 mem_service 也没有跨三节点的 consensus、leader election、replicated journal、
quorum barrier 或 failover ownership。不能把三个本地 daemon 宣称为一个一致的
distributed memory service。

### 5.5 W5 serving 是验证入口，ds4-server 已有可调用 API

当前 serving queue：

- 输入是文本文件中的 `prompt_token_ids`；
- 请求按顺序执行；
- nodeA ingress 是 guest control path；
- 没有 HTTP/gRPC/OpenAI API；
- 没有 tokenizer/chat template 服务契约；
- 没有 continuous batching、并发调度、取消、deadline 和背压；
- 没有稳定 SSE/WebSocket token stream；
- 没有多租户隔离、认证和配额。

W5 serving 可以证明状态机，不是可供用户调用的 infer service。ds4-server 已经补齐：

- OpenAI Chat/Responses/Completions；
- Anthropic Messages；
- SSE reasoning/text/tool-call streaming；
- tokenizer、chat template、DSML tool mapping；
- prefix reuse、disk KV cache 和 session persistence。

剩余服务差距是：单 graph worker 串行推理、没有 continuous batching；缺少正式认证、
租户配额、分布式 readiness、SLA 和生产化运维。

### 5.6 ds4 GGUF 与 W5 官方 checkpoint 不是同一部署制品

这是方案中最重要的产品选择：

- W5 loader 直接读取官方 Safetensors，payload 约 159.6 GB；
- ds4 只接受该项目发布并验证的特定 GGUF；Q2 imatrix 约 81 GB，Q4 imatrix
  约 153 GB；
- ds4 明确不是通用 GGUF loader，也不能直接加载任意 DeepSeek/官方 checkpoint；
- Q2 GGUF 与官方权重的 logits 不应被要求 bit-exact 相同。

因此验证顺序应是“Q2 单机/分布式 canary -> Q4 三节点目标服务”。W5 用于验证模型
结构、layer/KV/route 不变量；ds4 自己的 official continuation/logit vector 用于量化模型
数值验收。如果产品要求是“必须直接服务官方 Safetensors 原权重”，则 ds4 不能直接
满足，必须保留原评估中的 W5-native loader/CUDA backend 工作流。

### 5.7 现有 NVIDIA NVFP4 checkpoint 的作用和限制

三台机器的共享模型资产中已有一份完整 NVIDIA DeepSeek V4 Flash NVFP4 checkpoint：

- 46 个 Safetensors shard，目录约 158 GB；
- NVIDIA Model Optimizer 格式，routed experts 使用 NVFP4、group size 16；
- expert weight 存为 `U8`；
- `weight_scale` 为 `F8_E4M3`，另有 scalar `weight_scale_2` 和 `input_scale`；
- 模型卡声明的直接 serving runtime 是 SGLang/vLLM。

它不能被当前 `ds4-server` 直接加载，因为 ds4 production loader 只接受特定 GGUF。
当前 `deepseek4-quantize` 也不能原样转换这份 NVIDIA NVFP4：现有 expert source path
期待 `I8` packed weight、`.scale` `F8_E8M0` 和 group size 32，与 ModelOpt NVFP4 的
U8/E4M3/group-16 layout 不同。源码中的 `DS4Q_TYPE_NVFP4` 只是 GGUF type metadata，
当前 traits 将其标记为不可量化输出，并不代表 ModelOpt NVFP4 input adapter 已完成。

因此有三条互斥路径：

| 路径 | 判断 | 代价/影响 |
| --- | --- | --- |
| 下载/复制 ds4 发布的 Q4 GGUF | 推荐 | 最快进入 ds4 三节点 pipeline，不改模型代码 |
| 为 ds4 quantizer 增加 ModelOpt NVFP4 input adapter | 可做，非 MVP | 需实现 U8/E4M3/group-16 反量化，并做 compare-tensor、全量转换和质量回归 |
| 用 SGLang/vLLM 直接服务 NVFP4 | 独立备选架构 | 绕开 ds4/W5 runtime；当前模型卡证据来自更大 Blackwell 和 TP4/TP8，三台 Spark 仍需单独验证 |

现有 NVFP4 checkpoint 的直接价值是省去 SGLang/vLLM 路线的模型下载；它不会自动
缩短 ds4 Q4 三节点路径，除非完成并验证新的输入转换 adapter。

### 5.8 W5 official checkpoint 验收尚未闭环

截至本评估：

- 两节点 1-step 已真实通过；
- 两节点 4-step 的四轮 43 层计算和两端 guest pass 实际完成；
- 第 4 个 token 是换行符，raw text 把 QEMU marker 拆行，导致验收计数
  `expected=4 actual=3`，正式 run 仍返回失败；
- 三节点 official checkpoint 的 1-step/4-step 尚无通过证据；
- 8-step、MTP 和 1M context 尚未完成。

补齐该 baseline 仍然有价值，但不应阻塞 ds4 的单机和三节点部署验证。它验证结构与
状态机，不是 ds4 Q2/Q4 GGUF 的 bit-exact 数值 oracle。

## 6. 推荐目标架构

### 6.1 总体结构

```text
Client
  |
  v
ds4-server coordinator (Node A, MVP single leader)
tokenizer / OpenAI+Anthropic API / SSE / sampling / layers 0:14
  |
  +--TCP hidden over direct 200 GbE rail--> ds4 worker B
  |                                         layers 15:28 + local KV
  |                                              |
  |                         TCP hidden ----------+
  |                         v
  +<--TCP logits/result--------------- ds4 worker C
                                       layers 29:output + local KV

Node A ds4-server          Node B ds4 worker          Node C ds4 worker
  |                             |                           |
local mem_service           local mem_service           local mem_service
  |                             |                           |
local NVMe GGUF             local NVMe GGUF             local NVMe GGUF
```

MVP 采用 pipeline parallel，不引入 tensor parallel 或 expert parallel。原因：它与当前
W5 layer ownership 语义一致，迁移面最小，而且每个 token 的跨节点主要 payload 是
hidden handoff，不需要 all-to-all。

### 6.2 计算层：直接采用 ds4

不新增 `w5-dgx-worker`。现有执行边界已经是：

```text
ds4 GGUF loader
-> ds4_engine / ds4_session
-> ds4_session_eval_layer_slice()
-> ds4 CUDA backend
-> GB10 GPU buffers and kernels
```

部署工作只增加 wrapper、配置、preflight、evidence 和 mem_service adapter，不复制 ds4
模型内核。数值验收以 ds4 自带 CUDA regression、official vectors 和单/三节点 token
一致性为主；W5 用于结构和状态机交叉验证。

### 6.3 网络层：先使用 ds4 TCP，再按证据决定是否升级

第一阶段使用 ds4 现有 point-to-point TCP：

- Node A coordinator 监听 control socket，并执行 layers `0:14`；
- Node B worker 注册 layers `15:28` 及自己的 data address；
- Node C worker 注册 layers `29:output` 及自己的 data address；
- activation 按 `A -> B -> C` 直接转发，C 将 logits 返回 A；
- 所有地址显式绑定同一组直连 rail 的 IP；
- 开启 ds4 debug/decode profile，记录每 hop eval、wait、send 和 bytes。

选择 TCP 的原因不是它性能最优，而是实现已经存在，且单 token hidden payload 为
65,536 bytes；对 correctness MVP，减少新代码比预先优化 transport 更重要。TCP 不会
自动使用 RoCE verbs，也不会聚合双 rail。

当 profile 证明 host staging/TCP latency 已限制 TPOT 或 prefill，再评估两个方向：

1. 在 ds4 protocol 下增加 multi-rail TCP connection striping；
2. 保留 ds4 route/session protocol，替换 payload transport 为 NCCL P2P 或
   UCX/libfabric/ibverbs，并补 GPUDirect。

mem_service 的 TCP/Unix wire 继续只用于 control/metadata，不承载 decode hot path tensor。

### 6.4 mem_service 部署边界

MVP 每节点一个本地 daemon：

```text
worker <-> unix socket <-> local mem_service
```

建议职责：

- local KV/execution artifact metadata；
- model/session binding；
- version/checksum/provenance；
- idempotency/audit/metrics；
- checkpoint catalog 和本地 recovery。

不建议在 MVP 中做：

- 把三个 journal 拼成分布式一致性存储；
- 用 TCP block fetch 代替 NCCL hidden handoff；
- 在请求热路径同步三份 metadata；
- 宣称 worker crash 后无损接管 KV。

MVP 允许 Node A coordinator 是单点；生产 HA 另立阶段。

### 6.5 checkpoint 放置

目标三节点 pipeline 推荐每台 NVMe 保存同一份完整 ds4 Q4 imatrix GGUF，约 153 GB，
但 worker 只 map 自己拥有的 layer range。Q2 canary 也采用相同放置方式。原因：

- 每台有约 2.8 TiB 可用空间，完整复制成本可接受；
- 避免一开始引入共享文件系统或远程按需读取；
- 节点重分区和调试更简单；
- model ID、quant profile 和 GGUF checksum 可以完全一致。

ds4 已支持按 layer slice 的 tensor span mapping。第一版不需要切三份 GGUF；稳定后如需
进一步减少磁盘和启动成本，再使用 ds4 已支持的 split-model 机制生成部署制品。

## 7. 容量与性能初判

### 7.1 内存

目标 ds4 Q4 imatrix GGUF 约 153 GB。三节点按层切分后，平均权重约 51 GB；每台约
121 GiB 统一内存，所以“本地 layer slice 权重 + KV + graph scratch + runtime”理论上
有容量余量。完整 GGUF 可以保留在每台 NVMe，由 ds4 只 map 当前 slice 所需 tensor。
Q2 约 81 GB，单节点即可容纳，不应以容量为理由强制三节点。

但不能用简单平均数作为 admission：

- Node A 额外拥有 embedding/prompt 路径；
- Node C 额外拥有 final norm、output head 和 logits；
- 不同 layer 的 routed expert tensor 体积和实际 cache 命中不同；
- CUDA context、TCP staging buffer、KV、prefix cache 和 serving queue 需要安全余量；
- GB10 是统一内存，CPU/GPU oversubscription 会引入 UVM page fault 和不可控 tail latency。

实现时必须按 tensor metadata 计算每个 range 的真实 bytes，并用峰值显存/统一内存观测
决定 layer boundary。当前 `[15,14,14]` 只适合 correctness baseline，不保证性能均衡。

### 7.2 网络

W5 与 ds4 的 Flash shape 均为 `n_hc=4`、`n_embd=4096`；默认 FP32 wire 下单 token
hidden 为 65,536 bytes。ds4 默认 4096-token distributed prefill chunk 的单 hop hidden
约 256 MiB，prefill 需要真实 TCP 吞吐，decode 则主要受每 hop 启动延迟、同步、GPU/CPU
staging 和 pipeline bubble 影响。ds4 也支持 16-bit/8-bit activation wire；16-bit 可以
作为网络对照项，8-bit 必须单独做质量验收。

当引入多请求 microbatch 后，带宽利用率会上升，但仍应优先避免：

- host bounce buffer；
- 每 token 重建 communicator；
- 全局 barrier；
- 在 hot path 做 durable fsync；
- 把 tensor 编码成 text-kv RPC。

### 7.3 预期性能声明边界

在完成当前 commit 的三节点 end-to-end test 前，不能给出集群 tokens/s 或 TTFT 承诺。
ds4 已记录单台 Spark 在 7047-token Q2 workload 上 prefill 343.81 tokens/s、generation
13.75 tokens/s；distributed decode 每 token 多两个 worker hop，不能把单节点数字直接
乘以三。W5/macOS/QEMU 的小时级运行时间只反映 simulator correctness，也不可用于外推。

## 8. 关键差距与优先级

| 优先级 | 差距 | 必须完成的工作 | 对用户的影响 |
| --- | --- | --- | --- |
| P0 | 三节点 CUDA 实证缺失 | 在已通过的 Node A 基线上扩展到两/三节点 CLI | 单节点通过不等于 distributed 通过 |
| P0 | 模型制品选择未冻结 | ds4 Q4 GGUF、ModelOpt NVFP4 direct runtime 或 W5 official | 三条路径的 runtime、质量和工作量不同 |
| P0 | Node B/C 被占用 | 维护窗口；停止/迁移旧 workload；统一镜像 | 当前只有 Node A 能继续实跑 |
| P0 | ds4 TCP rail 未验收 | 绑定单条直连 rail；iperf3；ds4 per-hop profile；断链测试 | decode 时延和长 prefill 吞吐未知 |
| P1 | NVFP4 与 ds4 格式不兼容 | 采用发布版 Q4 GGUF，或另立 ModelOpt input adapter | 现有 158 GB 权重不能直接给 ds4 使用 |
| P1 | 三节点 W5 baseline 缺失 | 修日志转义；跑 official 3-node 1/4-step；固定结构/状态机 evidence | 减少跨 runtime 结构回归风险，但不阻塞 ds4 canary |
| P1 | 容器版本不一致 | 固定 ds4 commit、image digest、CUDA/cuBLAS 和 GGUF checksum | 避免节点行为和数值不一致 |
| P1 | ds4 服务集群验收缺失 | coordinator `ds4-server` + 两 worker；OpenAI/Responses SSE smoke | API 已实现但未在三节点 Spark 证实 |
| P1 | 调度能力有限 | 评估单 graph worker queue；补 backpressure/deadline/admission | 并发请求会排队，不能承诺多租户吞吐 |
| P1 | mem_service 跨机边界不足 | 明确 local sidecar；补必要 control RPC；不承担 hot tensor data | 避免把测试 transport 当生产数据面 |
| P1 | 内存规划缺失 | ds4 layer slice resident bytes、KV/context budget、OOM fail-closed | 防止统一内存抖动或 OOM |
| P2 | 故障恢复未产品化 | 验证 worker reconnect、prefix replay、snapshot、coordinator restart | 机制已存在但恢复时间和边界未知 |
| P2 | 安全缺失 | API auth/TLS；distributed control/data network isolation | ds4 distributed protocol 本身无认证/加密 |
| P2 | HA 缺失 | leader failover、catalog replication、session/KV recovery policy | MVP 有单点，不可宣称高可用 |
| P2 | 长上下文/MTP 未验收 | 使用 ds4 现有 KV/MTP 能力做独立容量、质量和稳定性测试 | 不能仅凭功能存在宣称服务规格 |
| P2 | RDMA/multi-rail 未实现 | 仅在 TCP profile 证明必要后扩展 ds4 transport | 不阻塞 MVP，但可能限制最终 SLA |

## 9. 分阶段落地方案

### Phase 0：冻结部署制品和证据边界

目标：避免把 W5 官方权重与 ds4 Q2/Q4 GGUF 混成同一数值制品。

任务：

1. 冻结已实测的 `ds4@80ebbc3` 和 Q2 canary GGUF checksum；
2. 决定三节点 target 是 ds4 Q4 GGUF，还是转向 SGLang/vLLM direct NVFP4；
3. 冻结容器 digest、CUDA/cuBLAS/toolchain；
4. 安排 Node B/C 维护窗口并确认 admission；
5. W5 继续补 official 3-node 1/4-step，作为结构/状态机 oracle；
6. ds4 数值正确性使用项目 official vectors，不要求与 W5 量化前 logits bit-exact。

### Phase 1：单节点 DGX Spark CUDA 基线

目标：在 Node A 完成当前 commit 和 Q2 GGUF 的 GB10 release evidence。

状态：server startup 已通过；request-level correctness 和 benchmark 尚未形成证据。

任务：

1. 固化已通过的 build/startup 命令、binary hash 和启动日志；
2. 执行 `make cuda-regression`；
3. 用 Q2 跑 HTTP 1-token/4-token、短 chat 和 7047-token benchmark；
4. 记录启动 resident bytes、峰值统一内存、prefill/decode、token 和 trace；
5. 验证无 CPU fallback。

现有 CLI：

```text
make cuda-spark
make cuda-regression
./ds4 --cuda -m <gguf> --nothink -p <prompt> -n 4
./ds4-bench --cuda -m <gguf> --prompt-file <prompt-file> --gen-tokens 4
```

### Phase 2：两节点到三节点 distributed CUDA

目标：用现有 ds4 protocol 完成三台物理机 pipeline correctness。

任务：

1. 先按 `0:21`、`22:output` 跑两节点 1/4-step；
2. 再按 `0:14`、`15:28`、`29:output` 跑三节点；
3. 三个进程显式绑定同一条直连 rail 的 IP；
4. 使用 `--debug` 和 decode profile 收集 route/per-hop bytes/timing；
5. 验证 worker-to-worker 直连、prefix hash、KV replay、worker restart 和断链；
6. 用 4096-token chunk 验证 pipelined prefill，再比较 16-bit activation wire。
7. Q2 correctness 通过后换 Q4 GGUF，重新做 resident bytes、1/4-step、长 prefill 和
   official vector 验收。

现有 CLI 形态：

```text
# Node A
./ds4 --cuda -m <gguf> --role coordinator --layers 0:14 \
  --listen <node-a-rail-ip> <control-port> --nothink -p <prompt> -n 4

# Node B
./ds4 --cuda -m <gguf> --role worker --layers 15:28 \
  --listen <node-b-rail-ip> <data-port> \
  --coordinator <node-a-rail-ip> <control-port>

# Node C
./ds4 --cuda -m <gguf> --role worker --layers 29:output \
  --listen <node-c-rail-ip> <data-port> \
  --coordinator <node-a-rail-ip> <control-port>
```

### Phase 3：stream infer service 与 mem_service 集成

目标：用户通过现有 OpenAI/Anthropic API 收到稳定 SSE token。

任务：

1. 将 Node A coordinator 从 `ds4` CLI 换为 `ds4-server`；
2. 验证 `/v1/responses`、`/v1/chat/completions`、Anthropic Messages 和 SSE；
3. 增加集群 preflight、启动、停止、status 和 smoke CLI；
4. 为三个本地 mem_service 发布 model/session/KV snapshot/artifact metadata；
5. 明确 ds4 disk KV 是 payload source of truth，mem_service 只做 catalog/provenance；
6. 增加 bounded admission、readiness、metrics 和结构化 evidence report。

必须补的项目 CLI/测试入口：

```text
w5-dgx cluster preflight --config <cluster-config>
w5-dgx cluster start --config <cluster-config>
w5-dgx cluster status --config <cluster-config>
w5-dgx service smoke --endpoint <url>
w5-dgx cluster stop --config <cluster-config>
```

### Phase 4：生产资格

目标：从 correctness MVP 提升到可承载受控业务流量。

任务：

1. 单 graph worker 的并发排队、admission 和容量上限；
2. TTFT/TPOT/tokens-per-second/capacity benchmark；
3. worker kill、网络断链、单 rail 失败、checksum corruption；
4. 重启、checkpoint reload、session abort/retry；
5. API TLS/auth 和 distributed 网络隔离；
6. systemd/container lifecycle、Prometheus/Alertmanager；
7. 24h/72h soak；
8. 明确支持的 context、batch、并发和 SLA。

continuous batching、multi-rail、NCCL/RDMA、GPUDirect 和 HA 都应由测试数据驱动，
不作为首个可调用服务的前置实现项。

## 10. 粗略工作量

以下是 1--2 名熟悉 Linux/CUDA、ds4 和当前 W5/mem_service 的工程师，在模型已可用、
三台机器有维护窗口的前提下的量级估算，不是排期承诺：

| 阶段 | 估算 |
| --- | ---: |
| Phase 0：制品/环境冻结 | 0.5--1 个工作日 |
| Phase 1：补齐单节点 request/benchmark evidence | 0.5--1 个工作日 |
| Phase 2：Q2 两/三节点 + Q4 三节点 distributed CUDA | 3--7 个工作日 |
| Phase 3：server + mem_service + lifecycle MVP | 3--7 个工作日 |
| Phase 4：生产资格 | 3--6 周 |

单节点 startup 已完成，因此基于 ds4 Q4 的首个三节点可调用服务合理量级仍是 1--3 周，
但前置不再包含 CUDA backend bring-up。Node B/C 释放后，若 distributed 路径没有
backend-specific bug，最快可在 2--4 个工作日完成 Q2 CLI 级 4-step 和基础 API smoke。
Q4 制品准备、容量/质量验收及生产化需要额外时间，生产资格仍需 3--6 周。

只有在产品硬性要求直接加载 W5 官方 Safetensors、保持当前 FP8/FP4 checkpoint 语义时，
才回到 W5-native CUDA backend 路线，其 correctness MVP 仍是约 8--14 周量级。

## 11. 决策门禁

只有逐项通过，才能进入下一阶段：

### Gate A：制品与正确性边界

- 已实测 ds4 commit/Q2 GGUF checksum 和 image/binary provenance 冻结；
- 明确 Q2 是 canary、Q4 GGUF 是 ds4 三节点目标；NVFP4 direct serving 另立架构；
- W5 结构/状态机 oracle 与 ds4 official vector 数值 oracle 分离；
- 真实 chat prompt，且无 CPU fallback。

### Gate B：单 DGX runtime

- GB10 CUDA startup 已通过；
- `make cuda-spark` 和 `make cuda-regression` pass；
- Q2 单节点 1/4-step、chat 和 benchmark pass；
- 资源空闲满足 admission；
- 峰值统一内存可控。

### Gate C：三节点 TCP pipeline

- 三节点同一 commit/image/Q4 GGUF；
- 单 rail TCP 和 ds4 per-hop profile pass；
- 1/4-step、长 prefill、KV replay pass；
- worker disconnect/reconnect 行为可预测；
- 不把未使用的 RoCE/RDMA/multi-rail 写入 SLA。

### Gate D：服务可用性

- 文本 prompt 到 SSE token 闭环；
- OpenAI Responses/Chat 和 Anthropic smoke；
- bounded admission/readiness；
- health/readiness/metrics；
- failure injection 和 soak pass。

## 12. 最终建议

建议继续，但不再按“新建 DGX native runtime”立项。应按“部署并验证 ds4 三节点 CUDA
service，再接入 W5 oracle 与 mem_service”推进。

最直接路径是：

1. 固化已通过的 ds4 commit、Q2 imatrix GGUF、启动日志和 binary/image provenance；
2. 用 Q2 补 CUDA regression、HTTP 1/4-step 和 benchmark evidence；
3. Q2 两节点/三节点通过后，按 `0:14`、`15:28`、`29:output` 验收 Q4；
4. 在已通过 startup 的 Node A `ds4-server` 上验证 OpenAI/Anthropic/SSE；
5. mem_service 保持每节点本地 sidecar，只接 metadata/artifact/KV snapshot catalog；
6. 根据 ds4 per-hop profile 决定是否开发 multi-rail 或 RDMA transport。

当前最大的阻塞已经不是 GPU backend，而是 Node B/C 被现有 workload 占用、目标 Q4
GGUF 尚未冻结，以及 ds4 三节点 CUDA distributed 尚无本环境实测证据。现有 NVIDIA
NVFP4 checkpoint 不能直接替代 ds4 Q4 GGUF。最需要避免的错误是为了保持 W5 代码边界
而重复实现 ds4 已经具备的 CUDA、distributed 和 server 能力。

## 13. 仓库证据索引

- `docs/plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md`
- `docs/2026-07-14-w5-deepseek-v4-flash-official-checkpoint-loader-report.md`
- `docs/w5_manual_serving_run.md`
- `docs/mem_service_independent_deployment_assessment.md`
- `docs/mem_service_target_status_gap_report.md`
- `guest-linux/aarch64/scripts/run_w5_cluster_config.sh`
- `guest-linux/aarch64/scripts/run_llm_infer_eight_node_guest.sh`
- `guest-linux/aarch64/scripts/w5_serving_entry.py`
- `guest-linux/aarch64/components/mem_service/mem_service_deepseek_v4_flash.c`
- `guest-linux/aarch64/components/mem_service/mem_service_daemon.c`
- `guest-linux/aarch64/components/mem_service/README.md`
- `crates/sim-chipbackend-simpler/src/lib.rs`
- `crates/sim-uapi/src/lib.rs`
- `ds4@80ebbc3:README.md`
- `ds4@80ebbc3:AGENT.md`
- `ds4@80ebbc3:Makefile`
- `ds4@80ebbc3:ds4.c`
- `ds4@80ebbc3:ds4_cuda.cu`
- `ds4@80ebbc3:ds4_distributed.c`
- `ds4@80ebbc3:ds4_server.c`
- `ds4@80ebbc3:tests/cuda_long_context_smoke.c`
