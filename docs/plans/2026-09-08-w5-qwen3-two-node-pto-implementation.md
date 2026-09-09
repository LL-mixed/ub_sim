# W5 Qwen3 两节点 PP：PTO 与 Lingqu shared-memory 实施记录

## 目标与验收范围

在 n4-910c 上运行 Qwen3-0.6B 两节点 PP。两个节点负责的模型数值计算
通过 Simpler/PTO 执行；共享 tensor 的 GM 参数绑定为 `UB_GM`，通过 PTO
`TLOAD/TSTORE` 访问 `lingqu_shmem` 的实际 backing。Memory Service 继续负责
对象身份、placement、lease、版本与发布；token sampling 保留在控制逻辑中。

RMSNorm 是内部检查点。完整交付覆盖 embedding、各 decoder layer 的 norm、
projection、RoPE、GQA attention、KV 更新与复用、residual、MLP，以及 final
norm 和 LM head。任何未迁移的数值计算都必须出现在覆盖报告中，禁止静默回退。

基线与候选使用相同权重、prompt、PP 分层、decode steps 和采样配置。
运行证据保存在独立 `out/` campaign 中，包含配置、源代码及构建指纹、逐节点日志、
算子覆盖、数值对比、退出状态和进程清理记录。本机只运行轻量检查；构建、模型、
QEMU 与完整回归放在 n4-910c。旧 P3 campaign 保持暂停。

## 已核实的起点

- W5 `qwen3_dense` 目前进入 Rust reference range forward。
- W5 `pipeline_double` 在 base dispatch 后执行，属于共享内存机制探针。
- UB_GM dispatch v2 已具备多 memref、scalar、authorization 与 callback 的
  基础表示；公开入口目前限定 callable 1 和三个 FP32 tensor。
- PTO CPU `TLOAD/TSTORE` 已覆盖连续与已验证的 strided ND/DN/NZ 访问。
- 独立 Qwen3/Simpler 路径可提供编排参考；其现有 tensor backing 为 host buffer。
  部分 Qwen3 示例 attention 使用 CANN extern，仍需适配 PTO CPU 仿真执行。
- Qwen3 hidden producer/consumer 按 FP16 编解码，部分对象 metadata 将该 payload
  标成 F32。模型 tensor 契约必须消除这一歧义。
- n4-910c 的已有仓库、Qwen3-0.6B 权重和构建工具可供复用。SSH 存在间歇性
  banner timeout；失败连接重试，禁止据此推断运行结果。

## 实施顺序

| 步骤 | 必须产出的行为 | 验证 |
| --- | --- | --- |
| 1 | 冻结基线，明确有效 token 数、dtype、shape/stride 与 KV 布局 | 元数据与真实字节一致；基线两节点 decode |
| 2 | 增加有明确参数契约和 artifact fingerprint 的模型 callable | 合法参数通过；错误 dtype、shape、access、scalar 拒绝 |
| 3 | 真实输入 RMSNorm 进入模型计算路径 | PTO 输出逐元素比较；Q/K/V 消费该结果；保留 residual 输入 |
| 4 | projection、Q/K norm、RoPE、attention 与 KV 链接 | 单层 prefill/decode 数值对比；旧 KV 复用、新 KV 写入 |
| 5 | residual、MLP、embedding、final norm、LM head 接入 | 完整算子覆盖；无 reference 数值计算回退 |
| 6 | 两节点完整 PP 验收与回归 | 逐层/最终 logits 对比、固定采样 token 对比、访问与 cleanup gate |

共享输入直接绑定已提交对象的有效数值区间。共享输出在本地 Memory Service
arena 分配，完成后原地发布。内部 tile 与节点私有临时张量按其实际生命周期
分配；不要求将每个临时值发布为跨节点对象。权重初始化及布局转换需要单独计量，
不能把每次 dispatch 的 host payload staging 计入 direct-access 成功。

## 完成条件

1. 两节点各自完成其全部 layer range，8 个推理轮次成功（首轮 prefill，
   后续 7 轮单 token decode）。
2. 覆盖报告列出每类算子的实际 PTO 执行次数，所有约定算子均被覆盖。
3. 真实 upstream hidden 进入下游算子，真实 KV 在后续 decode 被复用。
4. 被声明为 direct-access 的 tensor 不进入 host payload staging；对应
   `TLOAD/TSTORE` 地址、范围、访问权限与 Memory Service 对象一致。
5. 对固定测试输入报告逐层与最终数值误差、logits/token 对比。误差阈值根据
   运算 dtype 与累加方式预先定义，不能依据候选结果放宽。
6. mapping 与 lease 存活到 completion；失败路径禁止发布部分输出。
7. 规定回归通过，campaign 自身无 QEMU 残留，源代码与构建指纹可追溯。

当前状态（2026-09-09）：本节约定的功能目标已通过 R10 验收。全部模型
数值路径接入 PTO；n4-910c 真实两节点 PP 完成 8 个推理轮次，逐算子
次数、逐层 hidden/KV、全词表 logits、输出 token 均通过独立审计。
Hidden 与 KV 都在 PTO 输出 backing 原地发布；16 次 KV 发布的复制
字节数为 0，14 次后续 KV 解析与此前发布的对象身份一致。
mem_service、PTO 与 ub_sim 规定回归通过，本次 QEMU 无残留。
上游提交及 lock/gitlink 已同步；最终接线源码、测试与本记录一并提交，未 push。
下文保留各轮实现与失败记录，最新完成证据见末节 R10。

## PTO 数值实现约定

模型算子放在现有 `libs/lingqu_shmem_pto/` 的 `qwen3_pto_ops.hpp` 中，
与 guest 的 region/lease 适配代码分开编译。该实现由 Simpler kernel 调用；
独立 CPU 测试复用同一份代码。所有 GM 数值读写经 `TLOAD/TSTORE`，
FP16 边界经 `TCVT`，归一化、投影、RoPE、attention 与 MLP 采用 PTO tile
计算。标量循环只负责索引、切片和编排，禁止用 reference 数值结果替代输出。

独立算子测试验证普通 GM 与 synthetic UB_GM callback 的数值一致性、
访问范围及读写计数。这一层测试只证明算子实现；W5 真实对象绑定、
两节点传递及 8 步推理需要独立的最终端到端证据。

## 2026-09-08 实现与验证进展

本节记录主循环接线前的阶段结果。PTO 数值原语、完整 decoder layer、
embedding 与终端 logits 在该阶段通过了独立检查。主循环现已完成接线，
两节点实跑及当前审计状态见文末「两节点模型实跑与协议审计」。

| 层次 | 实现位置 | n4-910c 验证结果 | 证据覆盖边界 |
| --- | --- | --- | --- |
| 数值原语 | `qwen3_pto_ops.hpp` | GM / UB_GM 两模式通过 | RMSNorm、Linear、RoPE、Attention、Softmax、Residual、SwiGLU、Copy/转换 |
| decoder layer | `qwen3_pto_layer.hpp` | 独立 oracle 比较通过 | 小尺寸非均匀权重；prefill 与带历史 KV 的 decode；4 Q heads / 2 KV heads |
| 模型两端 | 同上 `embedding()` / `terminal_logits()` | 独立 oracle 比较通过 | 非连续 token、tied embedding/LM-head、最终 norm、非法 token 禁止部分写出 |
| Simpler callable | `qwen3_pto_kernel.cpp` / `qwen3_pto_orch.cpp` | 8 个操作全部 Success | 真实 bridge/Simpler/PTO，mock backing；不包含 QEMU guest |
| Rust 全量回归 | `cargo test --workspace` | 1,002 passed / 16 ignored，退出码 0 | 当前任务 Rust 修改；忽略用例仍按测试声明跳过 |
| W5 两节点 8 步 | guest W5 range flow | 已接入；实跑见文末 | 独立记录实际 guest 证据 |

运行入口：

```text
python3 guest-linux/aarch64/scripts/run_qwen3_pto_ops_tests.py \
  --compiler <native-cxx> --pto-isa-root <pto-isa> --output-dir <evidence-dir>

cargo run -p sim-cli -- qwen3-pto-operator-check \
  --manifest <qwen3_pto_operator_manifest.json> --scenario <simpler-capi.yaml>
```

单层及模型两端测试保存在 `out/qwen3-pto-layer-r2/report.json`。
GM / UB_GM 两组都通过；UB_GM 组累计 1,912 次读、61 次写 callback，
读取 228,134 bytes、写入 72,894 bytes。这些计数包含全部原语及组合测试。
数值门限在测试中预先固定为 `3e-5 * (1 + abs(expected))`，oracle 使用独立
标量/double 计算，candidate 使用 PTO tile 指令。小尺寸正确性证据不证明
真实 Qwen3-0.6B 权重上的误差、吞吐或完整模型输出。

Bridge 证据为 `out/qwen3-pto-bridge-report.json` 和对应 `.log`。
八个操作最大绝对误差 `2.384185791015625e-7`，低于固定 `3e-5` 门限；
每个操作都有非零读写 callback 和一次 fence。运行入口继续强制检查
`host_payload_bytes() == 0`。该次检查没有 QEMU guest，也没有真实 OBMM
Memory Service 对象，因此不将它记为共享对象端到端验收。

### 剩余集成工作及约束

1. 模型级 callable 3 已完成编译与真实权重单 range 验证，结果见下节。
   W5 guest 主推理循环现已切换到该 callable；实跑与协议审计见文末。
2. 模型级入口已提供真实权重及 RoPE 常量。已核对远端模型配置：28 layers、
   hidden 1024、intermediate 3072、16 Q heads、8 KV heads、head_dim 128、
   vocab 151936、RMS epsilon `1e-6`、RoPE theta `1000000`。
   配置声明 tied embeddings；权重包同时存在 `lm_head.weight`，运行时优先
   使用显式 head，不根据配置假定其字节与 embedding 相同。
   权重初始化与私有 GM 成本单独记账，禁止把共享 hidden/KV staging 隐藏在其中。
3. Guest 从 Memory Service 获取真实 hidden / previous-KV view，为输出申请
   local arena lease，绑定完整数值区间。`next_key/next_value` 需要读写权限，
   因为同一层的 attention 要消费刚追加的 KV。旧 KV 与新 KV 分开分配；
   output completion 成功前不得发布，lease 保持到完成。
4. 将 W5 原 range 数值调用替换为 PTO 结果驱动的流程，保留对象发布、版本、
   token sampling 等控制逻辑。现有 `pipeline_double` probe 不参与验收。
   需要移除候选路径对 reference embedding、layer forward、terminal logits
   的调用，禁止候选失败后回退生成成功输出。
5. 在同一权重、prompt、采样和两节点 PP 配置下运行完整 8 个推理轮次，
   核对逐层 hidden/KV、最终 logits/token、算子执行覆盖和实际 shared-memory
   访问，最后完成完整回归与本 campaign 的 QEMU 清理。

完整 Python 回归第一次执行为 401 项，1 failure / 41 errors。多数报错来自
隔离工作区缺少 pinned QEMU/kernel/libobmm 源码；已补齐对应 gitlink 版本。
同时发现 OBMM test launcher 在验证缺失输入前触发 QEMU 构建，已把输入验证
前移，补充禁止该错误路径触发构建的断言。补齐后的第二轮 403 项通过；
包含 launcher 新断言的第三轮也以退出码 0 完成，日志单独保存在
`out/qwen3-pto-python-tests-r3.log`。第一次失败日志仍保留在远端。
本轮未启动任务所属 QEMU guest，未触碰既有外部 QEMU；旧 P3 PID 419618
仍为暂停状态。工作区 Rust 日志为 `out/qwen3-pto-workspace-tests.log`。

## 模型级 callable 与真实权重检查

新增 callable 3：六个 guest UB_GM memref、十二个几何参数，以及由 host
初始化的第七个私有 GM 权重/常量参数。guest 参数覆盖 hidden、previous KV、
token 元数据、hidden output、next KV 和 last-token logits。其 C++ 编排可
执行指定 layer range；完整 range 同时覆盖 embedding 和最终 norm/LM head。

在 n4-910c 使用真实 Qwen3-0.6B 权重、token IDs `[1, 42]`，通过
`qwen3-pto-range-check` 完成以下验证。candidate 通过 bridge/Simpler/PTO 执行；
reference 在 candidate 完成后独立运行。backing 为检查器的 mock 区域，
没有启动 QEMU guest，不能将此结果计为 W5 两节点通过。

| 指标 | 单层 `[0,1)` | 完整 `[0,28)` |
| --- | ---: | ---: |
| 状态 | pass | pass |
| hidden 最大绝对误差 | 0.00168085 | 0.1640625 |
| hidden 最大归一化误差 | 0.000312360 | 0.000451853 |
| KV 最大归一化误差 | 3.59196e-7 | 3.73422e-5 |
| reference 候选 logits 最大绝对误差 | 不执行 terminal | 1.81198e-5 |
| 全词表 top-1 | 不执行 terminal | 1038，与 reference 一致 |
| UB_GM read / write callbacks | 6,193 / 36 | 173,377 / 3,328 |
| shared payload staging | 0 bytes | 0 bytes |

归一化误差定义为 `abs(candidate-reference)/(1+abs(reference))`。固定门限：
hidden `0.002`、KV `0.0002`、候选 logits 绝对误差 `0.02`。完整结果中
hidden 绝对误差包含 FP16 边界量化；报告同时保留原始绝对误差，避免只展示
归一化指标。当前 logits 检查覆盖全词表有限性、top-1 和 reference top
candidates 的数值；全词表逐元素误差尚未提供。

证据：`out/qwen3-pto-real-full-range-r2.json` 及对应 `.log`。完整 range
初始化了 `3006530560` bytes 私有权重/常量；此项与共享 payload staging
分开计量。初次完整测试因拒绝显式 LM head 而失败，失败日志保留于远端
`out/qwen3-pto-real-full-range-r1.log`；修正后使用新构建目录重新验证。

新增小尺寸数值回归覆盖独立 LM head、完整 prefill、终端 range decode、
旧 KV 保留/新 KV 追加及 KV header 不被 kernel 覆盖。普通 GM 与 UB_GM 均
通过，报告为 `out/qwen3-pto-range-golden-r1/report.json`。该测试使用零投影
权重构成 residual identity，并用独立 head oracle 检查参数布局；非零
projection 的数值验证继续由既有单层测试与真实权重检查承担。

### Guest 接线状态

`qwen3_pto_guest.h/.c` 已实现准备/提交/释放接口，接入 W5 Makefile 和
initramfs 构建入口。n4-910c 完整 guest 可执行文件构建成功，日志为
`out/qwen3-pto-w5-guest-build-r1.log`。metadata 页先写入再查询物理地址，
避免尚未驻留的匿名页导致 pagemap 查询失败。

`test_qwen3_pto_guest.py` 两项检查通过：真实 lease/memref/wire 实现配合
fake provider/MMIO，覆盖六个 lease、token 边界检查、成功返回 arena 输出、
失败禁止暴露输出、超时保持 lease；另检查两条构建入口。证据为
`out/qwen3-pto-guest-contract-r1.log`。超时后再查询 completion 的恢复接口
尚未实现；现状保留资源并拒绝释放，不宣称已完成超时恢复。

W5 主推理循环已接线：保留真实 upstream hidden/previous KV view，绕过
reference 数值 dispatch，使用 PTO 输出驱动发布与 token 控制逻辑。审计
还发现当前 Memory Service range publisher 对 KV 无条件分配新 block 后
复制；要使新 KV 直接写入最终发布区间，需要将 KV 输出分配和发布位置
衔接起来，不能只复用已有的 hidden 原地发布开关。KV 原地发布仍待完成；
当前带 guest arena 内复制的两节点 8 步功能验证已经通过。

### 本轮回归

- `cargo test --workspace -- --test-threads=1` 在 n4-910c 退出码 0。
  顶层结果共 949 passed / 16 ignored；隔离子进程还输出 63 个单用例通过
  记录，不能将其与顶层统计相加后称为独立用例总数。日志为
  `out/qwen3-pto-workspace-range-r2.log`。
- 首轮并行回归在 Engram 启动自检出现 `Text file busy (os error 26)`；
  该失败保留于 `out/qwen3-pto-workspace-range-r1.log`。串行全量重跑通过，
  没有删掉或跳过该测试；并行冲突的根因仍未独立复现定位。
- Python full discovery 403 项通过、退出码 0，日志为
  `out/qwen3-pto-python-range-r1.log`。随后新增的 guest helper 两项测试
  单独运行通过，见 `out/qwen3-pto-guest-contract-r1.log`。
- `cargo fmt --all -- --check` 与 `git diff --check` 通过。
- 未启动任务所属 QEMU guest；现有外部 QEMU 未触碰，旧 P3 PID 419618
 仍为 `Tl`。本轮没有提交或推送。

## 两节点模型实跑与协议审计

### 固定运行配置

- 运行机器：n4-910c；隔离目录 `ub_sim_w5_qwen3_pto_20260908`。
- 模型：真实 Qwen3-0.6B 权重；两节点 PP 分层 `[0,14)` / `[14,28)`。
- Prompt token IDs：`81378,37585,374`；greedy / top-k=1。
- 共 8 个推理轮次，编号 0..7：第 0 轮处理 3-token prefill，后续 7 轮
  每轮处理 1 token。此处“8 steps”指 8 轮并产生 8 个输出 token。
- 候选模型 callable=3，执行链为 W5 guest → sim-qemu bridge → Simpler
  → PTO kernel；共享 hidden、KV 与 logits 通过 UB_GM `TLOAD/TSTORE`。
- 显式关闭可选 UB-SSD/GSVA backend materialization。Memory Service
  继续使用 OBMM shared-memory backing；没有将 GVA/GSVA 纳入默认功能集。

### 本次发现并修复的问题

| Campaign | 观测结果 | 处理 |
| --- | --- | --- |
| `w5-qwen3-pto-2n-8step-r1` | nodeA 完成数值计算后，未使用的 logits 可写占位参数在 fence 阶段失败 | 非末 range 的 logits 占位参数改为只读；guest、Rust contract 与 Simpler orchestration 同步，并增加负向契约测试 |
| `w5-qwen3-pto-2n-8step-r2` | nodeA 成功发布；nodeB 收到可选 SSD backend 的本地读缓冲区，memref acquire 返回 `-EINVAL` | 专用 runner 选择 OBMM 对象视图，禁用该可选 materialization 路径 |
| `w5-qwen3-pto-2n-8step-r3` | 两节点各 8 轮成功，runner exit=0，模型 fingerprint 前后一致 | 独立参考比较确认所有 token/top-4 排序一致；进一步协议审计发现 text checksum 未组合 step/token 等字段 |
| `w5-qwen3-pto-2n-8step-r4` | 修正协议后，两节点各 8 轮成功，runner exit=0；独立审计 `status=pass` | token、top-4 排序、logits 误差与全部候选文本元数据通过；QEMU 成功完成和 staging gate 通过 |
| `w5-qwen3-pto-2n-8step-r5` | 添加逐算子入口计数后重新构建模型 artifact，两节点各 8 轮成功；独立审计 `status=pass` | 每轮各算子次数符合实际几何参数，并与成功完成记录匹配；token/数值/元数据审计继续通过 |

R3 的模型数值输出已经通过两节点闭环。其 text checksum 使用了 tokenizer
piece checksum，缺少 W5 token-result 协议组合；不能用数值一致掩盖该元数据
差异。guest 现已复用等价的 W5 checksum 公式（单次 range 的 byte offset 为 0），
增加固定样例测试；新镜像 R4 已通过实跑与独立审计。失败证据均保留，禁止覆盖。

### R4 实测结果

参考 campaign 为 `w5-qwen3-reference-2n-8step-r1`，同样完成两节点 8 轮、
退出码 0。下表的两条路径每轮 top-4 token 和排序完全一致，文本 checksum、
piece 长度与内容也一致。32 个候选 logits 的最大绝对误差为
`4.38690185546875e-5`，低于固定门限 `0.02`。

| 轮次 | PTO / reference 相同输出 token | Top-4 最大绝对误差 |
| ---: | ---: | ---: |
| 0 | 264 | 5.722046e-6 |
| 1 | 3644 | 4.386902e-5 |
| 2 | 7653 | 2.861023e-5 |
| 3 | 304 | 3.433228e-5 |
| 4 | 279 | 2.002716e-5 |
| 5 | 2070 | 1.716614e-5 |
| 6 | 315 | 3.242493e-5 |
| 7 | 1995 | 2.002716e-5 |

| 实际 QEMU callback 计数 | nodeA `[0,14)` | nodeB `[14,28)` |
| --- | ---: | ---: |
| 成功模型 dispatch | 8 | 8 |
| 旧 KV resolve | 7 | 7 |
| UB_GM load bytes | 17,432,616 | 17,473,536 |
| UB_GM store bytes | 5,984,256 | 10,846,208 |
| 成功 fence | 16 | 24 |
| shared payload host staging | 0 | 0 |

这些是实际完成记录中的累计字节，包含 attention 对 KV 的重复读取，
不能解释为唯一 tensor 容量。nodeA 每次只需 fence hidden 与 next-KV；
nodeB 还需要 fence logits。16 个 request 全部 `completion_success`，
每次 6 个 bindings 都在完成后 unbind。

本地汇总证据为
[R4 运行报告](../../out/w5-qwen3-pto-verification-r4/report.json) 和
[参考对比审计](../../out/w5-qwen3-pto-verification-r4/reference-audit-r1.json)。
完整 guest/QEMU 原始日志保留在远端隔离仓库的 `guest-linux/aarch64/logs/`
下对应 campaign；审计报告记录各日志 SHA-256。

构建与输入 SHA-256：

| 对象 | SHA-256 |
| --- | --- |
| Qwen3 `model.safetensors` | `f47f71177f32bcd101b7573ec9171e6a57f4f4d31148d38e382306f42996874b` |
| 模型 `config.json` | `660db3b73d788119c04535e48cf9be5f55bc3100841a718637ae695b442f27dd` |
| `tokenizer.json` | `aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4` |
| callable 3 manifest | `c906fad95e75047689a8d9864a5d0528172e631a9153cf6809889d7ffd9398ab` |
| QEMU binary | `f7c99fc1ae2e4fda837dcdc3fce0e9cea08e7284d6464de6222f275da3df3b0d` |
| kernel Image | `ad6ea6082df3eeb32ac31f034fb309e42c4b0257aa74bdb51e7b4e37994b6087` |
| R4 基础 initramfs | `2ff72546c59c2dafaa50b587d61f75d1ec144d3d680d480993cf4faa050f1c5c` |

基础 initramfs 与 runner 每次生成的带 token table/run_app 的运行镜像应区分；
表中未把基础镜像 hash 声称为最终运行镜像 hash。模型 manifest 在 R4 运行前后
查询得到相同 fingerprint。

本次最终代码回归：Python full discovery `409 passed`，日志
`out/qwen3-pto-python-text-r1.log`；Rust `cargo test --workspace -- --test-threads=1`
退出码 0，日志 `out/qwen3-pto-workspace-text-r1.log`。Python 这轮耗时 244 秒，
其中子模块版本描述触发部分克隆历史拉取；最终正常结束，未终止或跳过用例。

### 可重复入口

```sh
python3 guest-linux/aarch64/scripts/run_w5_qwen3_pto.py \
  --manifest out/qwen3-pto-range-artifacts-r4/qwen3_pto_range_manifest.json \
  --weights /path/to/Qwen3-0.6B \
  --scenario out/qwen3-pto-operator-scenario.yaml \
  --run-id <unique-run-id>

python3 guest-linux/aarch64/scripts/audit_w5_qwen3_pto.py \
  --candidate-logs guest-linux/aarch64/logs/<candidate-run>_headless8 \
  --reference-logs guest-linux/aarch64/logs/<reference-run>_headless8 \
  --output out/<unique-audit-report>.json
```

Runner 默认两节点、8 steps。scenario 必须选择 `simpler_capi`；运行环境的
compiler、kernel、initramfs、QEMU 路径通过单独 `.env` 文件配置。
审计器检查每个 node/step 的 layer range、token/past-KV 几何、hidden 原地
发布、旧 KV 的 OBMM 解析、16 个 request 的成功完成、读写/fence 与零 shared
staging，并比较全部 8 轮的 top-4 logits、token 与文本元数据。其固定 logits
绝对误差门限为 `0.02`；输出包含输入日志的 SHA-256，已有输出文件禁止覆盖。

### R5 逐算子实跑证据

计数直接在实际函数入口递增，每个 range 开始清零，并使用 thread-local
存储。审计要求每轮都存在计数记录，且该 dispatch 的最终 completion 成功。
下面统计包含嵌套调用，例如 attention 中逐行 Linear 与 Softmax；数值代表
算子函数调用次数，不能解释为 PTO ISA 指令条数。

| 算子入口（8 轮累计） | nodeA | nodeB |
| --- | ---: | ---: |
| Embedding | 8 | 0 |
| Decoder layer | 112 | 112 |
| RMSNorm（含 Q/K norm、末端 norm） | 448 | 456 |
| Linear（含 attention 内部矩阵计算） | 5,264 | 5,272 |
| RoPE | 280 | 280 |
| Attention（逐 Q head） | 1,792 | 1,792 |
| Softmax（逐 Q head / token） | 2,240 | 2,240 |
| Residual | 224 | 224 |
| SwiGLU | 112 | 112 |
| Copy / dtype 转换 | 326 | 316 |
| Terminal logits（含 final norm / LM head） | 0 | 8 |

R5 的 8 个输出 token、top-4 排序、文本元数据与 R4 相同；top-4 最大绝对
误差仍为 `4.38690185546875e-5`。每节点的 UB_GM load/store/fence 计数
与 R4 一致。运行前后 callable fingerprint 均为 `0xce83c7e7d01f7ada`。
本次 campaign 无 QEMU 残留；主机上两个既有外部 QEMU 保持不动，未据此
实跑作性能结论。

证据为 [R5 运行报告](../../out/w5-qwen3-pto-verification-r5/report.json)、
[R5 算子与参考审计](../../out/w5-qwen3-pto-verification-r5/reference-audit-r1.json)
及同目录 fingerprint 文件；完整原始日志保留在远端对应 campaign。
新增回归检查遗漏计数、错误次数、不同 token/past 几何及 thread-local 隔离。
该版代码在 n4-910c 的 Python full discovery 为 409 tests / OK（25.302 秒），
Rust 全 workspace 串行回归退出码 0；日志分别为
`out/qwen3-pto-python-counts-r1.log`、`out/qwen3-pto-workspace-counts-r1.log`。

### 验证边界与剩余项

#### R6：真实 guest 数值快照与未通过项

新增 `run_w5_qwen3_pto.py --numerical-trace` 和
`sim-cli qwen3-pto-trace-check`。前者在独占的新 campaign 目录采集快照，
后者独立重建参考两节点计算、历史 KV 与参考 FP16 节点间传递，不消费候选
的 hidden/KV 作为参考计算输入。模型依然从真实 shared-memory 对象获取输入。
诊断快照通过额外 PTO load 记录各层 hidden/KV；logits 在正常 `TSTORE`
成功后直接观察输出 tile，未给只写 binding 增加读权限。默认关闭诊断。

R6 功能运行退出码 0，callable fingerprint 前后一致；既有逐算子、completion、
top-4/token/文本元数据审计通过。16 个文件完整记录了 224 次 layer 执行及
8 轮全词表。严格数值报告为 **fail**，结果如下：

| R6 数值项 | 比较元素数 | 最大归一化误差 | 门限结果 |
| --- | ---: | ---: | --- |
| 每层 hidden | 286,720 | 0.001372341881506145 | 通过 `0.002` |
| 每层完整 K（含历史行） | 1,490,944 | 0.0002519808185752481 | 超过 `0.0002` |
| 每层完整 V（含历史行） | 1,490,944 | 0.0007408762467093766 | 超过 `0.0002` |
| 每轮完整 logits | 1,215,488 | 0.00017879663209896535 | 最大绝对误差 `0.00023174285888671875`，通过 `0.02` |

所有超限元素均位于 nodeB；共 94 个 step/layer/K-or-V 检查有超限，累计
1,725 次元素比较超限。历史 KV 会在后续轮次被再次比较，这个数目不能解释
为 1,725 个不同的 KV 元素。8 个 top-1 token 仍全部与独立参考一致。
原始快照和失败报告保留在
[R6 数值审计](../../out/w5-qwen3-pto-verification-r6/numerical-audit-r2.json)
旁的 `numerical-trace/`，首次遇到超限即停止的日志也单独保留。

已找到并正在验证的算术差异：PTO CPU `TRSQRT` 先以 double 计算倒数平方根，
再舍入到 FP32；W5 参考的 FP32 sqrt 和除法分别舍入。候选 RMSNorm 现采用
PTO `TSQRT + TDIV`，明确匹配参考的两步舍入。新增 exact-FP32 golden 在
GM/UB_GM 两模式通过，原有独立 double oracle、快照完整性和禁止覆盖测试
继续通过。R7 使用新模型 artifact 重跑完整 guest，修正效果需以 R7 审计
结果确认；R6 的门限和失败记录不改写。

R7 已完成：两节点 8 轮功能和执行审计通过，严格数值审计仍为 `fail`。
两步舍入对齐使超限检查从 94 项降到 21 项；首轮全部通过，全部 K 通过。
剩余超限位于后续 decode 的 nodeB 第 25～27 层 V；最大 V 归一化误差为
`0.00036183767952024937`，全部 hidden 最大归一化误差为
`0.0008460545795969665`，全词表最大绝对误差为
`0.0001430511474609375`。报告见
[R7 数值审计](../../out/w5-qwen3-pto-verification-r7/numerical-audit-r1.json)。

进一步代码核查发现模型常量生成器的 RoPE 使用 FP64 角度和三角函数，
参考 `apply_rope()` 使用 FP32 角度及 `sin_cos()`。已对齐常量生成的
FP32 运算顺序，增加能区分旧 FP64 结果的逐 position/column 回归；
该修改涉及 Rust bridge，使用项目 wrapper 重建 QEMU，再以 R8 新 campaign
验证。R8 两节点各 8 轮功能、逐算子次数和协议审计通过；严格数值审计继续
返回 `fail`，门限保持原值：

| R8 检查 | 实测最大误差 | 门限 | 结果 |
| --- | ---: | ---: | --- |
| 逐层 hidden，归一化 | `0.0008460545795969665` | `0.002` | pass |
| 全部 K，归一化 | `0.0002564485475886613` | `0.0002` | fail |
| 全部 V，归一化 | `0.0002361298684263602` | `0.0002` | fail |
| 全词表 logits，绝对值 | `0.0001571178436279297` | `0.02` | pass |

20 次超限观察均来自 nodeB：step 1 的第 26/27 层 V 各一个元素，在后续
step 2～7 的历史 KV 中持续被比较；step 2 的第 14 层 K 有一个超限元素，
在 step 3～7 持续被比较。因此这些失败涉及 3 个不同的历史 KV 元素。
8 个输出 token 与参考一致，全部 32 个 top-4 logits 最大绝对误差为
`0.000057220458984375`。记录与快照见
[R8 执行审计](../../out/w5-qwen3-pto-verification-r8/reference-audit-r1.json) 和
[R8 数值审计](../../out/w5-qwen3-pto-verification-r8/numerical-audit-r1.json)。

该版本的 Rust 全 workspace 串行回归退出码 0；Python full discovery
409 tests / OK，28.874 秒。QEMU wrapper 确认 Rust staticlib 更新并执行
relink，R8 使用的 QEMU SHA-256 为
`8d65ab38ccfa1fdae98181a22414f808433fa354b266ba18986b19d068025955`；
模型 artifact manifest SHA-256 为
`7969827fe7ce5a75d07431ecf3aad7cd4a2da3aa5844cf286bd97d170c695fae`。
检查时 campaign 自身无残留 QEMU，两个既有外部 QEMU 保持不动。

R8 的 nodeA 第 13 层 pre-FP16 hidden 最大归一化误差约为 `1.46e-5`，
nodeB 第 14 层最大误差约为 `3.88e-4`。这提示需要单独量化 FP16 边界
影响。检查器已增加 `handoff_diagnostics`：将候选与参考的 pre-FP16 值
使用同一参考舍入规则转换，列出转换后不一致的元素。该项仅用于诊断量化
边界跨越，不能当作已观察到的 guest wire bytes，也不向独立参考计算注入
候选值。诊断报告不修改原有数值 gate，重新审计结果单独保存。

[R8 边界诊断报告](../../out/w5-qwen3-pto-verification-r8/numerical-audit-r2.json)
已生成，各轮使用同一参考舍入规则后仍不一致的元素数依次为
`38, 27, 17, 21, 17, 12, 15, 15`。首轮有 3,072 个 hidden 元素，
后续各轮有 1,024 个。这些统计证明量化边界跨越确实存在；仅凭此项诊断
尚不能把全部 KV 误差归因到边界。

具体样例为 step 0、hidden 索引 208：候选 pre-FP16 为
`-1.9165050983428955`，参考为 `-1.9165035486221313`；同一舍入规则
分别得到 `-1.9169921875` 与 `-1.916015625`。转换前差值约 `1.55e-6`，
转换后差值为 `0.0009765625`。下一步追踪量化前的 FP32 运算差异，
包括 PTO CPU `TEXP` 的 double-exp 再转 float 与参考 FP32 exp 的差别；
代码中确实存在这一实现差异，其对模型误差的贡献尚未通过隔离实验确认。
诊断不构成放宽门限的理由。

增加边界诊断后的 Rust 全 workspace 串行回归退出码 0，包含新的量化
边界跨越单元测试；Python full discovery 409 tests / OK（29.280 秒）。
R8 数值重审依然报告原有 20 次 KV 超限观察，证明增加诊断没有掩盖原失败。
日志与报告保存在上述 R8 本地证据目录。检查器源代码 SHA-256 为
`6601920d6c581799f623f046d981fec8282635ae8c5ae2212e3aff96acce990d`，
重审 JSON SHA-256 为
`ef4960a92526f718fe4bbc6201bf174826198b27f4d5e3da9abe0425d6de1fae`。

```sh
python3 guest-linux/aarch64/scripts/run_w5_qwen3_pto.py \
  --manifest <model-manifest> --weights <weights-dir> \
  --scenario <simpler-capi.yaml> --run-id <unique-run-id> --numerical-trace

target/release/sim-cli qwen3-pto-trace-check \
  --trace-dir out/w5-qwen3-pto/<run-id>/numerical-trace \
  --weights <weights-dir> --tokens 81378,37585,374 --steps 8
```

数值报告与独立 `audit_w5_qwen3_pto.py` 的成功执行报告必须同时成立。
数值审计发现超限时输出结构化 `failures` 并以非零退出；缺少快照、记录错序、
截断、非有限值、额外文件和缺少结束标记都会失败。这轮快照运行不作性能结论。

#### R9：完整模型严格数值审计通过

隔离精度测试先使用原 PTO 库，在 16,384 个确定性有限 FP32 输入中观测到
8 个 `TEXP` 结果与 FP32 `exp` overload 不同，旧测试以非零退出。
`vendor/pto-isa/include/pto/cpu/ElementOp.h` 的 float 分支现直接调用
FP32 overload，其他类型分支保持原行为；CPU `texp` 增加逐 bit 比较测试。
Qwen3 的 GM/UB_GM golden 同时覆盖该回归。两组 golden 与独立指令级
精度测试均通过。旧失败日志保存为 `out/qwen3-pto-exp-before-r1/gm.log`。

使用新 callable artifact（`qwen3-pto-range-artifacts-r7`）的 R9 已在
n4-910c 完成真实两节点 PP、8 steps。QEMU 使用 R8 的已重链版本，
本轮仅更换编入 PTO float-exp 修改的模型 kernel。结果如下：

| 检查项 | 实测最大误差 | 原固定门限 | 结果 |
| --- | ---: | ---: | --- |
| 逐层 hidden，归一化 | `0.00047340866876766086` | `0.002` | pass |
| 全部 K，归一化 | `0.00007283078593900427` | `0.0002` | pass |
| 全部 V，归一化 | `0.00010834366548806429` | `0.0002` | pass |
| 8 轮全词表 logits，绝对值 | `0.00005173683166503906` | `0.02` | pass |
| W5 原始日志 top-4 logits，绝对值 | `0.00002574920654296875` | `0.02` | pass |

8 个输出 token 仍为 `264, 3644, 7653, 304, 279, 2070, 315, 1995`，
与参考一致。每轮各算子实际次数与 R5 表格一致，两节点各 8 次成功 dispatch、
各 7 次旧 KV resolve。开启快照后累计 load bytes 分别为 23,396,392 与
23,437,312；store bytes 分别为 5,984,256 与 10,846,208，成功 fence
分别为 16 与 24。诊断 load 的增加单独记录，不用于性能判断。

`handoff_diagnostics` 在全部 8 轮均为 0 个差异：使用同一参考舍入规则
转换后的 nodeA hidden 相同。该结果与 KV 全部通过共同支持 FP32 exp
差异会影响后续量化及模型误差的判断；此诊断仍不等价于 guest wire 字节
逐 bit 相同，也不要求所有中间 FP32 值逐 bit 相同。

证据：[R9 运行报告](../../out/w5-qwen3-pto-verification-r9/report.json)、
[执行审计](../../out/w5-qwen3-pto-verification-r9/reference-audit-r1.json)、
[逐层与全词表审计](../../out/w5-qwen3-pto-verification-r9/numerical-audit-r1.json)。
同目录保存 16 个原始数值快照；完整 guest/QEMU 日志保留于远端 campaign。
运行前后 callable fingerprint 一致，数值检查进程退出码为 0。

| R9 构建对象 | SHA-256 |
| --- | --- |
| PTO `ElementOp.h` | `fcb09f0c3b582c28e86718179937c93e56421ce15de0ef90416087c74c158dcd` |
| callable manifest | `b7fc7bd373f5f862c5d4523f43ff5d33990f2c19152671c3f12e94a29a787c95` |
| kernel binary | `4c49e83fd87623facd39b7412e595e36bda714629d10a2788c8932b29159faec` |

本轮 Rust 全 workspace 串行回归退出码 0，Python full discovery 为
409 tests / OK（23.909 秒）。PTO 库独立精度测试通过；完整 CPU ST 程序
已全部构建完成（构建目录 `out/pto-full-exp-r1`）。首次运行因 test runner
未显式指定已安装的 GCC 15 路径而停止；单独的 `texp` runner 在数据生成
阶段缺少 numpy。两次启动日志保留，均不计为测试套件通过。独立 Python
环境已在 `out/pto-test-venv` 准备完成：Python 3.11.16、numpy 2.4.6、
ml-dtypes 0.6.0、en-dtypes 0.0.4。后续运行显式传入 `--cxx` 和 `--cc`。
R9 验证当时 PTO 变更尚未提交，gitlink 尚未更新；
本记录中的源码/二进制 hash 用于标识已测工作区，不能当作已提交版本证明。
后续完整 CPU 回归发现并修复了 GM FIFO 越界，完整清单核对结果见文末。

#### 独立 mock-backing 数值补充

`qwen3-pto-range-check` 已增加全词表逐元素比较。在独立 reference 计算时
选择保留完整 logits；普通 reference 推理入口继续只返回摘要。比较器检查
精确长度和双方有限性，覆盖 top-4 以外的值，并沿用预先固定的 `0.02`
绝对误差门限，超限即失败。新增单元测试覆盖跨 chunk 的 token 顺序、摘要
不变性、非候选 token 错误、长度不匹配及非有限值，均在 n4-910c 通过。

真实权重完整 `[0,28)` range 使用与 W5 相同的 prompt
`81378,37585,374`，n4-910c 实测如下：

| 数值检查 | 实测结果 | 固定门限 |
| --- | ---: | ---: |
| 全词表比较元素数 | 151,936 | 必须等于完整 vocab |
| 全词表最大绝对误差 | 0.00002765655517578125 | 0.02 |
| 最大误差对应 token ID | 18,169 | 所有 token 均参与比较 |
| Top-1 | 264，与 reference 一致 | 必须一致 |
| 最终 hidden 最大绝对误差（FP16 输出） | 0.101715087890625 | 同时报告归一化误差 |
| 最终 hidden 最大归一化误差 | 0.00045399166992865503 | 0.002 |
| 全部层 KV 最大归一化误差 | 0.00005572200825554319 | 0.0002 |

该检查 `status=pass`，执行同一 R5 模型 artifact，经 bridge/Simpler/PTO
完成，读/写 callback 为 346,753 / 3,805，fence 为 3。完整报告为
[全词表数值报告](../../out/w5-qwen3-pto-verification-r5/qwen3-pto-full-vocab-oracle-r1.json)，
同目录保留原始日志、检查器源代码 SHA-256 与新增单元测试日志。
它使用 mock backing、单个完整 range、prefill 输入；没有 QEMU guest 和
节点间 FP16 handoff。因此这项通过与 R5 两节点实跑分别陈述；真实 guest
全部 8 轮的逐层/全词表快照对比现已由后续 R9 通过，见前述独立报告。

增加全词表检查后的完整回归也已在 n4-910c 通过：
`cargo test --workspace -- --test-threads=1` 退出码 0；Python full discovery
409 tests / OK（24.927 秒）。日志为同一证据目录下的
`qwen3-pto-workspace-full-vocab-r1.log` 和 `qwen3-pto-python-full-vocab-r1.log`。
本地 `cargo fmt --all -- --check` 与 `git diff --check` 通过。

1. 端到端日志中的 logits 数值对比覆盖每轮 top-4；R6～R8 另有实际 guest
   快照的全词表逐元素与逐层 hidden/KV 对比。R9 全部通过固定门限，
   历史失败保留，严格数值验收已具备成功证据。
2. 完整模型源码的每个 layer 顺序调用 norm、QKV、Q/K norm、RoPE、GQA、
   output projection、residual 与 MLP；末 range 执行 final norm / LM head。
   R5 已补齐各算子实际入口计数，并通过逐轮次数与 completion 关联审计。
3. R9 的 Hidden 在 PTO 输出 arena 原地发布。KV 数值读写由 PTO 对 OBMM arena
   完成；R9 发布时 Memory Service 仍把 next-KV 复制到独立 KV block。该复制发生
   在 guest 共享 arena 内，QEMU 的零 host staging 计数不包含也不能否认它。
   KV 最终 block 预留/原地发布在该阶段尚未完成；文末 R10 已完成独立
   `mem_service` 验证、提交与锁定集成，并关闭这个缺口。
4. 成功完成路径的 lease/unbind 有真实证据。超时后 completion 恢复 API、
   多 dispatch 并发和性能不由本次成功路径证明。

以上记录对应 R9 阶段：完整模型两节点功能与严格数值闭环已有成功证据，
KV 原地发布仍需集成。后续完成情况见 R10，历史结果不替代新 artifact 验收。

#### KV 最终区间原地发布：后续改动边界

数值 gate 已由 R9 通过。下一项实现沿用 Memory Service 已有的 KV block
分配规则，提前预留最终发布区间；不把普通 64-byte 对齐 arena buffer
冒充为最终 KV block。

| 层次 | 必要改动 | 证明条件 |
| --- | --- | --- |
| Memory Service 分配 | 在 PTO dispatch 前复用 KV tier/block-span 规则预留区间 | 对齐、容量、整数溢出、arena/slot 边界检查；失败不返回有效输出 |
| compute adapter / guest | 将预留区间绑定为 next-KV memref，保持读写 lease 到 completion | PTO K/V 地址与预留区间一致；失败或未完成禁止发布 |
| Memory Service 发布 | 增加显式 KV 原地发布契约，验证指针、offset 与 block span | 发布不再次分配或 memcpy；未选择新路径的调用继续保持原行为 |
| 验收 | 增加原地发布及负向 executable tests，再跑两节点 8-step | 后续 7 轮读取已发布 KV；执行、数值、复制计数与 cleanup 同时通过 |

2026-09-09 的上游开发已实现下列代码，尚未接入 W5：

- `mem_service_model_kv_state_validate_reserved()` 复用已有 tier/span 计算，
  在读取 payload 之前验证实际指针、block 对齐、完整 reserved span 的
  arena/slot 边界及地址溢出。失败不写结果、不增长 arena。
- range request 增加 `publish_kv_in_place` 和 `publish_kv_offset`；显式
  开启后发布复用已预留 offset，跳过 KV 分配和复制。默认值保持原复制路径。
  日志增加 `payload_mode` 和 `publication_copy_bytes`，用于实际 W5 审计。
- executable fixture 使用真实 allocator、publisher、record/ObjectRef
  实现，替代 host 上不可用的可见性/通知设施；截获显式 `memcpy`，
  检查复制次数、arena 增长与最终记录的 backing offset。

独立远端检出 `out/mem-service-kv-in-place-r1` 的验证状态：

| 检查 | 当前证据 | 范围 |
| --- | --- | --- |
| KV reservation / publication | `mem-service-kv-reserved-r3.log`：2 test methods / 23 scenarios，退出码 0 | 包括单/多 block、错指针、对齐、padding/slot/arena 边界、溢出、checksum、visibility failure；原地模式 0 次 payload copy，旧模式 1 次 |
| host build / fixture gate | `mem-service-kv-host-r1.log`：退出码 0 | `linqu_mem_service_host host-artifact-smoke`；不证明 guest 接线 |
| Python 全套回归 | `mem-service-kv-full-r2.log`：138 tests / OK，3 skipped，退出码 0，473.432 秒 | 使用隔离 Python 3.11；包含新增 KV 测试。跳过项按既有 standalone 仓库条件处理，不记作通过 |

首轮 full discovery 使用系统 Python 3.9，不能解析既有 PEP 604 注解；
既有 terminal visibility fixture 还缺少 UAPI include 路径。补齐 include
并切换 Python 3.11 后完成上述全套回归，失败日志 `mem-service-kv-full-r1.log`
继续保留。

独立核对了 3 个 skip：它们分别依赖 ub_sim 的 `build_initramfs.sh`、
`crates/sim-uapi/src/lib.rs` 和 `apps/llm_infer/llm_infer.c`，这些路径
不属于 standalone mem_service 检出。没有增加 skip 条件，也没有跳过新增
KV 用例；下游集成仍需在锁定新版本后单独验证。本地与远端被测的 4 个
组件源文件/头文件及 4 个测试文件 SHA-256 已逐项核对一致。

首次文件同步路径多了一层，发现 0 tests 的 `mem-service-kv-reserved-r1.log`
不计为通过；r2 因缺 UAPI include 路径编译失败；修正后的 r3 才是上述
23 场景的成功证据。所有旧日志保留。

上述 fixture 验证时，`mem_service` 改动尚未提交，W5 消费原锁定版本。
R9 的数值成功证据与本次独立 upstream fixture 分开保存。上游约束要求
先完成 mem_service 自身测试和明确授权的提交，再同步可获取提交、gitlink
与 lock；禁止用未提交的 mem_service 源码构建 ub_sim 绕过集成门禁。
当时的后续工作是将最终 KV reservation 接到 compute adapter，并重跑两节点
8-step 的访问、数值及零发布复制审计；这些集成条件现已由 R10 通过。

#### PTO 完整 CPU 回归发现的 GM FIFO 越界

依赖安装完成后，`tests/run_cpu.py` 的完整默认 CPU ST 在
`tpushpop_cv_nosplit` 发生 SIGSEGV，退出码 1；此前 `texp` 程序已通过。
失败日志为 `out/pto-full-exp-tests-r2.log`，不能把前半程通过记为完整通过。

独立 AddressSanitizer 运行定位到 `TPush_gm()` 构造的 GM tensor 使用
`TileProd::Rows/Cols`：物理 tile 为 `128×128`，有效数据为 `16×128`，
每个 FIFO slot 按有效区域分配 8192 bytes。描述符使用过大的物理 shape，
使 `TSTORE` 越过两个 slot 合计 16384 bytes 的 heap 区域。失败堆栈保留在
`out/pto-nosplit-asan-r1.log`。

修正位于 PTO CPU `TPush.hpp`：GM descriptor 的 shape/stride 使用
`GetValidRow()/GetValidCol()`，同时适用于静态和动态 valid shape；新增
`32×64` 物理 tile、`16×32` 动态有效区域测试。原用例继续保留。
`out/pto-nosplit-asan-r2.log` 记录 12/12 tests 通过，退出码 0，无 ASAN
越界报告。完整默认 CPU ST 已重新构建/运行成功，日志分别为
`out/pto-full-fifo-build-r3.log`、`out/pto-full-exp-tests-r3.log`，退出码 0。
独立 XML 核对与 CMake 的 `ALL_TESTCASES` 精确匹配：133 个程序，
无缺失、无额外程序；1,143 个用例，0 failures / 0 errors / 0 disabled。
其中 1,139 个通过，4 个 `ttrace` 用例因默认关闭 trace mode 跳过。
单独开启 trace mode 后，这 4 项已全部通过，退出码 0，XML 无 skip；
日志为 `out/pto-trace-mode-r1.log`。默认全套与单独补测的报告分开保存。
该完整默认配置为 BF16=OFF、trace=OFF；不包含 CMake 默认未启用的
`tmatmul_mx`，不据此宣称所有可选配置均已验证。

Qwen3 GM/UB_GM 数值复验 `out/qwen3-pto-fifo-golden-r1.log` 已通过，
包含原语、prefill、带历史 KV 的 decode、embedding/final logits 和
range 组合；UB_GM 累计读/写 callback 为 3555 / 92。
这些是独立算子/组合验证，仍需在最终接线后获取新 W5 artifact 的实跑证据。

该修改在上述回归时尚未提交。R9 是前述已标识 artifact 的模型数值证据；最新库修改的
完整回归结果及随后 W5 集成实跑需要分别记录，不能用 R9 替代新版本验收。

#### 2026-09-09 授权、依赖提交与最终 KV 接线

用户已授权提交本次 mem_service/PTO 修改和同步 lock/gitlink，不执行 push。

| 仓库 | 提交 | 内容 |
|---|---|---|
| mem_service | `c0f01389e276fd46794625a6e37d5df29a4bde18` | 预留 KV block 原地发布、校验和回归 |
| PTO | `2a2eb4dda8e3c4c86b3c5baa7a70e6bac15e3dc4` | FP32 TEXP 精度及测试 |
| PTO | `6a9da1e1e9571f98de1c43d88d58478804948410` | GM FIFO valid shape 越界修正及测试 |
| ub_sim | `65293bd900d759f9ba21b1edf651bccb5640e8a7` | 两个 gitlink 与 mem_service.lock 同步 |

上游通过 Git bundle 同步到 n4-910c，两个被消费的工作树均与提交完全一致。
公共 Git remote 尚未推送；跨设备验证使用已同步的 bundle，不宣称远端分支
已发布。所有提交包含英文说明和 Co-authored-by，message 行长不超过 78。

compute adapter 新增 `lingqu_shmem_mem_service_acquire_local_kv()`，复用
Memory Service 的 tiered allocator，再将有效 payload 区间绑定为读写
memref。分配器保留的 padding 不暴露给 kernel。Qwen3 第 4 号 memref
选择这个路径；completion 成功后，W5 请求以同一 offset 原地发布 KV。
不支持 KV reservation 的 backend 返回 `-EOPNOTSUPP`，无普通分配回退。

远端 `qwen3-kv-in-place-contract-r1.log`：11 tests / OK，退出码 0，
覆盖 lease、wire offset、KV 分配失败、backend 缺少能力、交叉编译，以及
发布 offset/复制计数的负向审计。补充的 next-decode 审计核对前一轮
ObjectRef 的 offset、长度、checksum、key hash 和 version。

完整 guest Python 回归 `qwen3-kv-in-place-python-full-r2.log`：409 tests /
OK，23.148 秒，退出码 0。第一次全套因远端父仓库索引未同步 gitlink
失败，保留 `qwen3-kv-in-place-python-full-r1.log`；补齐索引后原测试通过。
Rust `cargo test --workspace -- --test-threads=1` 退出码 0，日志为
`qwen3-kv-in-place-rust-full-r1.log`。新 artifact 的 R10 验收结果如下。

#### R10：最终 KV 原地发布与完整模型验收通过

运行位置为 n4-910c 隔离工作区，campaign
`out/w5-qwen3-pto/w5-qwen3-pto-2n-8step-r10`。使用新 callable artifact
`out/qwen3-pto-range-artifacts-r8/qwen3_pto_range_manifest.json` 和重新编译
的 guest initramfs。`report.json`、`reference-audit-r1.json`、
`numerical-audit-r1.json` 均为 `status=pass`，三个命令退出码均为 0。
本地完整证据保存在 `out/w5-qwen3-pto-verification-r10/`。

| 验收项 | R10 实测结果 |
|---|---|
| 模型 / 拓扑 | Qwen3-0.6B；nodeA 执行层 `[0,14)`，nodeB 执行层 `[14,28)` |
| 推理轮次 | 1 轮 3-token prefill + 7 轮单 token decode；每节点 8 次 dispatch |
| 算子覆盖 | 224 次 decoder layer；embedding 8 次、terminal logits 8 次；每轮各类算子计数匹配预定公式 |
| KV 原地发布 | 每节点 8 次，共 16 次；全部 `payload_mode=in_place`、`publication_copy_bytes=0` |
| KV 后续复用 | 每节点 7 次，共 14 次；offset、长度、checksum、key hash、version 与前轮发布一致 |
| 实际 GM 访问 | nodeA 读/写 23,396,392 / 5,984,256 bytes；nodeB 23,437,312 / 10,846,208 bytes |
| dispatch 完成 | 每次解绑 6 个 memref；nodeA/nodeB 累计 16/24 次 fence；无参考计算回退 |
| host staging | 每次 `segment_payload_staging_bytes=0`，每次模型 `shared_payload_staging_bytes=0` |
| 数值快照 | 16 个文件，224 份逐层记录，8×151,936 个 logits；`failures=[]` |
| 清理 | 本 campaign 的 QEMU 全部退出；两个原有外部 QEMU 未触碰；旧 P3 PID 419618 保持暂停 |

归一化误差定义为逐元素 `abs(actual-reference) / (1 + abs(reference))`，
再取全部被检查元素的最大值。门限沿用此前固定值，无放宽。

| 数值维度 | R10 最大误差 | 固定门限 | 结果 |
|---|---:|---:|---|
| 各层 hidden 归一化误差 | 0.00047340866876766086 | 0.002 | pass |
| 各层 K 归一化误差 | 0.00007283078593900427 | 0.0002 | pass |
| 各层 V 归一化误差 | 0.00010834366548806429 | 0.0002 | pass |
| 全词表 logits 绝对误差 | 0.00005173683166503906 | 0.02 | pass |

输入 token 为 `[81378, 37585, 374]`；8 轮输出
`[264, 3644, 7653, 304, 279, 2070, 315, 1995]` 与独立参考完全一致。
top-4 token、runner-up、文本片段 metadata 也通过对比。

| 产物 | SHA-256 |
|---|---|
| initramfs | `994d5e094b0dc1b1f910cd953cb9296fa009b79aa705f518f8d0099fcab14f59` |
| guest linqu_llm_infer | `a3e0d2fa4575d8a434256315f59c449e0557358c6b545584522729a32f962ea5` |
| callable manifest | `97d75ae665bcc9a4b24e6cc617bc3a288c5a2d43ac3ce55c73db74376b856c44` |
| QEMU | `8d65ab38ccfa1fdae98181a22414f808433fa354b266ba18986b19d068025955` |

artifact fingerprint 前后相同；产物及接线源码的 SHA-256 校验全部通过，
记录见 `artifact-sha256.txt` 与 `dependency-gitlinks.txt`。源代码 overlay
在 R10 运行时包含本任务待提交的主仓库修改，不能仅用远端父仓库旧 HEAD
复现本次运行。本次主仓库提交收录该模型实现、最终接线、测试与验收记录。
上游两个仓库已是干净提交，具体 hash 见前表。

提交完成后，本地 mem_service 的 daemon/wire-client 文件出现其它工作中的
未提交修改，已原样保留。n4-910c 被本次构建消费的 mem_service/PTO
工作树仍为上述干净提交；本次测试结论不覆盖那些随后出现的本地修改。

本次结论限于上述模型、配置、两节点串行 PP 和 8 轮功能验证。并发多
dispatch、超时后恢复、长期稳定性、其它模型配置以及性能优化仍各自需要
专项验收；它们不属于本次用户要求的两节点全算子功能目标。数值快照会增加
读访问，且主机存在外部 workload，本报告不从运行耗时推导性能结论。
