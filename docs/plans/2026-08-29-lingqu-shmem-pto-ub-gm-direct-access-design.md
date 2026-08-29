# 支持 PTO 通过`TLOAD/TSTORE` 直接访问 `UB_GM`

> 日期：2026-08-29
>
> 审计基线：`ub_sim` `299888a84a77`
>
> 范围：guest Linux、QEMU UBC、`sim-qemu`、`sim-uapi`、`sim-runtime`、
>`sim-chipbackend-simpler`、Simpler、PTO ISA、OBMM

## 1. 结论

现有 `host_vector`/ChipBackend workload 已经贯通以下执行链：

`guest IoOpcode::Dispatch` → QEMU UBC `linqu_uapi_kick()` →
`sim-qemu` bridge → `sim-uapi` → `sim-runtime` →
`sim-chipbackend-simpler` → Simpler/PTO callable。

因此，不需要创建一条从 attached-NPU opcode 进入 PTO 的并行执行链。正确的实施方向是在已贯通的 ChipBackend bridge 上增加 `UB_GM` 参数类型和 simulator UB GM memory interface，消除当前`segment_payloads → Vec<u8> → host pointer` 的 payload staging。

当前仓库中存在两条名称接近、语义不同的 vector 路径，分别为：

| 路径 | 当前真实执行链 | 已经证明 | 当前限制或剩余工作 |
| --- | --- | --- | --- |
| ChipBackend `host_vector` | QEMU UBC → `sim-qemu` → `sim-uapi` → `sim-runtime` → `sim-chipbackend-simpler` → Simpler/PTO | guest/QEMU bridge 能调起实际 Simpler/PTO callable；新增 `lingqu_shmem_memref` 分支已在两节点直接访问 UB GM backing；authorization pending/resume 已在同一两节点路径通过 | 传统 `host_vector` 参数继续使用 host payload staging；P4 负向、layout、并发和 P5 性能/上层集成仍待完成 |
| experimental `sim_npu` / `NPU_OP_VECTOR_ADD_U32` | QEMU `ub_npu.c` → `g_malloc()` → `ubc_gsva_device_read()` → C 循环 → `ubc_gsva_device_write()` | 可选实验路径中的 device CNA、GSVA acquire/read/write/fence 能工作 | 没有进入 Rust bridge、Simpler 或 PTO ISA；不属于默认 feature set |

设计决定：

1. 用户可见类型命名为 `lingqu_shmem_memref`。
2. PTO/Simpler 地址空间命名为 `AddressSpace::UB_GM`。
3. 继续使用现有 `IoOpcode::Dispatch`、QEMU UBC `linqu_uapi` ring 和 `sim-qemu` bridge。
4. 不新增 `NPU_OP_PTO_DISPATCH`，主路径不依赖 `ub_npu.c` command processor。
5. OBMM 负责 `lingqu_shmem` 的 alloc、export、import、映射和生命周期。
6. QEMU 为 PTO CPU simulator 提供通用 UB GM load/store/fence interface；具体 mapping、权限和一致性实现留在 QEMU adaptor 内部。
7. `sim-chipbackend-simpler` 把 `lingqu_shmem_memref` materialize 为 `ChipTensor(AddressSpace::UB_GM)` 和 dispatch-local binding。
8. PTO kernel 继续使用 `GlobalTensor<T>`、`TLOAD` 和 `TSTORE`；kernel 内不出现 OBMM payload API。
9. 数据 payload 不进入 `sim-uapi::segment_payloads`，控制 metadata 可以通过现有 UAPI ring 和版本化 control table 传递。
10. mapping、bounds、access、lifetime、ordering 或 callback 检查失败时 fail-closed。
11. `sim_npu`、GVA 和 GSVA 统一标记为 `experimental`、`optional`、`default disabled`；它们不进入默认 feature set，也不构成 UB GM direct-access 的架构依赖或验收前提。

目标架构如下。

![现有 ChipBackend bridge 上已贯通的 PTO UB_GM 架构](2026-08-29-lingqu-shmem-pto-ub-gm-architecture.svg)

## 2. 上位目标与“直接访问”的验收口径

Upstream 设计 [`linqu_data_system.md`](https://github.com/xwhu/pypto_workspace/blob/f43b084e281d/docs/pypto_top_level_design_documents/linqu_data_system.md) 规定：

- `lingqu_shmem` 提供 UB 网络上的分布式共享内存；
- L0–L2 将其映射到 GM space；
- `gm_tensor` 可以放置在该区域；
- PTO `TLOAD/TSTORE` 可以直接访问；
- 数据面不增加额外软件 copy。

文件名保留历史拼写 `linqu_data_system.md`，服务名使用`lingqu_shmem`。本文的新 API、ABI、日志和测试统一使用 `lingqu_*`。

### 2.1 “直接访问”的可验证定义

`TLOAD` 会把 GM 数据搬入 tile，`TSTORE` 会把 tile 数据写回 GM；这些 ISA 定义内的数据移动属于正常执行。本文的“直接访问”采用以下可观测定义：

- kernel 参数引用 `lingqu_shmem` 中的原始 tensor view；
- Simpler 参数被标记为 `AddressSpace::UB_GM`；
- PTO `TLOAD` 经 simulator UB GM memory interface 读取原始 backing；
- PTO `TSTORE` 经同一受控路径写回原始 backing；
- requester 使用配置给 PTO/ChipBackend 模拟设备的 device CNA；
- guest runtime、Rust bridge、`sim-uapi` 和 `sim-runtime` 不创建 payload 镜像；
- input 不执行 H2D，output 不执行 D2H；
- producer 从原始 `lingqu_shmem` mapping 观察到 consumer 的 PTO 写入结果。

### 2.2 不计入完成的路径

| 路径 | 不能作为完成证据的原因 |
| --- | --- |
| guest 先 `memcpy()` 到普通 tensor，再 dispatch | PTO 访问复制后的 tensor |
| `sim-uapi` 从 `segment_payloads` 取得 bytes 再构造 host `Vec` | payload 经 host staging |
| `prepare_simpler_capi_args()` 把 `Vec` pointer 填入 `Tensor::new()` | `ChipTensor` 仍指向 HOST memory |
| 把 guest EL0 VA 写入宿主 `ChipTensor.buffer.addr` | 宿主 PTO CPU simulator 不能解引用 guest VA |
| 把 QEMU host backing pointer 直接暴露给 PTO | 绕过 mapping、bounds、access、lifetime 和 ordering |
| 仅运行 experimental `NPU_OP_VECTOR_ADD_U32` | 覆盖可选 GSVA device operation，未覆盖 PTO callable |
| 仅运行 Simpler 自带的 POSIX SHM/global-domain 示例 | 未覆盖 `lingqu_shmem` mapping 和 QEMU UB GM interface |
| UB GM 失败后回退到 host copy | 破坏 fail-closed 与 no-staging 语义 |

控制 metadata 的复制不计为 payload staging。`h2d_bytes`、`d2h_bytes` 和
`segment_payload_staging_bytes` 必须分别为零。

### 2.3 Feature status 记录

本文记录以下 feature classification：

| Feature | Status | 默认 feature set | 对 UB GM direct-access 的地位 |
| --- | --- | ---: | --- |
| `lingqu_shmem_memref` + `AddressSpace::UB_GM` + PTO `TLOAD/TSTORE` | target | 包含 | 默认主目标 |
| existing QEMU UBC → `sim-qemu` → ChipBackend → Simpler/PTO dispatch | baseline | 包含 | 默认控制/执行主链 |
| `sim_npu` / QEMU attached-NPU semantic operations | experimental、optional | 不包含，default disabled | 独立实验与 regression |
| GVA | experimental、optional | 不包含，default disabled | 可选地址映射实验 |
| GSVA | experimental、optional | 不包含，default disabled | 可选 QEMU/OBMM adaptor 实验 |

该 classification 已进入实际默认配置：`vendor/qemu_8.2.0_ub@76942965`
要求显式 `UB_SIM_EXPERIMENTAL_FEATURES=npu` 才创建设备，隐式 GSVA route 同样需要
`gsva` token；`vendor/qemu_8.2.0_ub@3f54db3b` 进一步关闭默认 GSVA ARM-MMU
mode。专用 NPU/GSVA regression runner 会显式传入对应 token，普通 PTO UB GM
acceptance 不设置这些 token。

## 3. 当前实现审计

### 3.1 审计与实施输入

| 仓库或 submodule | Revision |
| --- | --- |
| `ub_sim` | `299888a84a77` |
| `guest-linux/kernel_ub` | `70c7272a8b66` |
| `vendor/obmm` | `53011eed1071` |
| `vendor/pto-isa` | `ecb6c303f797` |
| `vendor/qemu_8.2.0_ub` | `15951308ea8f` |
| `vendor/simpler` | `8a6a28f405c8` |
| `pypto_ws_hu_core` 上位设计 | `f43b084e281d` |

这些 revision 记录最初审计输入。随后完成的 P0–P3 实施证据如下；表中的 revision
均为已经提交的阶段性代码。P2 的同步与可恢复 authorization 正向路径已经完成，
P4/P5 继续保持未完成状态。

| 阶段 | 仓库 | Revision | 已提交内容 |
| --- | --- | --- | --- |
| P0 | `ub_sim` | `a05ca02` | `AddressSpace::UB_GM` C++/Rust ABI 与 layout contract |
| P0 | `ub_sim` | `ff25e17` | dispatch v2、memref、callback、binding、counter ABI |
| P0 | `vendor/simpler` | `d31717be` | `UB_GM = 2` C++ tensor address space |
| P0 | `vendor/qemu_8.2.0_ub` | `ace22c26` | default-disabled `pto-device-cna` property |
| P1 | `vendor/simpler` | `70350b51` | UB_GM tensor host-staging bypass |
| P1 | `ub_sim` | `0c52386` | UB_GM runtime arg、view、binding materialization 与 fail-closed tests |
| P1 | `vendor/pto-isa` | `594817f2` | CPU `TLOAD/TSTORE` checked callback 与 mock vector-add tests |
| P2 | `vendor/simpler` | `6cf285cb` | 跨 DSO 传播 UB GM worker-local run context |
| P2 | `ub_sim` | `70fb8b5`、`0903675` | Simpler callback adaptor 与 authorized bridge dispatch |
| P2 | `ub_sim` | `3b7945f`、`6eb8c22` | tag-10 slot ABI 与 QEMU ingress contract tests |
| P2 | `vendor/qemu_8.2.0_ub` | `0da2a94a`、`dc8d9633` | tag-10 ingress、同步 OBMM authorization、opaque OBMM map handle 与 request-scoped callback/binding |
| P2 | `vendor/qemu_8.2.0_ub` | `001eb084` | PTO worker callback 执行期间释放 QEMU BQL |
| P2 | `vendor/qemu_8.2.0_ub` | `915a7ebe` | authorization slot snapshot、memref cursor、monotonic sequence、CMDQ head 保留与 timer completion resume |
| P2 | `ub_sim` | `8ef0b95` | authorization delay/timeout CLI、pending/resume evidence gates、runner 修正与 QEMU gitlink |
| P3 | `ub_sim` | `5f88743`、`bc69fa1`、`81b157e` | guest dispatch builder、queue endpoint、opaque memref 与 lifetime |
| P3 | `ub_sim` | `504f411`、`18f55b2`、`c3bbe93` | 两节点 workload、initramfs wiring 与 acceptance CLI |
| P3 | `ub_sim` | `0770b01`、`f3a7e3f`、`182ef9b`、`b2abe0d` | import aperture、mapping sync、BQL callback 安全与 callable-1 对齐 |
| P3 | `vendor/qemu_8.2.0_ub` | `76942965`、`3f54db3b` | experimental NPU/GSVA 与 GSVA ARM-MMU 默认关闭 |
| P3 | `ub_sim` | `4f33311`、`53fd476` | 默认路径与 GVA/GSVA 解耦、契约 gate 与最终 QEMU pin |

### 3.2 已贯通的 ChipBackend/Simpler/PTO 主链

代码证据如下：

| 层 | 当前代码证据 | 结论 |
| --- | --- | --- |
| QEMU UBC bridge 初始化 | [`ub_ubc.c`](../../vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c) 的 `linqu_uapi_init_bridge()` 调用 `linqu_ub_bridge_new_from_yaml()` | QEMU 内已经嵌入 Rust bridge |
| guest descriptor ingress | `linqu_uapi_kick()` 读取 64-byte slot，调用 `linqu_ub_bridge_submit_slot()` 和 `linqu_ub_bridge_ring_doorbell()` | guest dispatch 进入 `sim-qemu` |
| `sim-qemu` FFI | [`ffi.rs`](../../crates/sim-qemu/src/ffi.rs) 暴露 new/register/submit/ring/poll C ABI | QEMU 与 Rust bridge 已可同步交互 |
| adapter | [`adapter.rs`](../../crates/sim-qemu/src/adapter.rs) 将 descriptor 送入 `LocalGuestUapiSurface` | descriptor 到达 `sim-uapi` |
| dispatch 路由 | [`sim-uapi/src/lib.rs`](../../crates/sim-uapi/src/lib.rs) 的 `IoOpcode::Dispatch` 调用 `run_chipbackend_dispatch()` | ChipBackend 路由已存在 |
| `host_vector` | `run_w4_chipbackend()` 选择 `run_host_vector_chipbackend()` | vector workload 沿主链运行 |
| runtime | `run_host_vector_chipbackend()` 构造 `BackendDispatchOperation` 并提交 `LocalRuntimeEngine` | 已进入 `sim-runtime` |
| Simpler C API | [`sim-runtime/src/lib.rs`](../../crates/sim-runtime/src/lib.rs) 加载 artifacts、创建 callable、调用 `run_prepared_callable()` | 实际进入 Simpler runtime/PTO artifact |

这条链已经回答了执行入口问题：PTO callable 由现有 QEMU bridge 和`sim-chipbackend-simpler` 执行。本文后续改动必须保留这条链。

### 3.3 当前主链的数据 staging

既有 `host_vector` 继续使用 host memory：

1. `sim-uapi::run_chipbackend_dispatch()` 从 `segment_payloads` 取得 guest input；
2. `run_host_vector_chipbackend()` 构造 `input_a_bytes`、`input_b_bytes`，并通过`seed_host_segment()` 写入 runtime 的 host segment；
3. `sim-runtime::prepare_simpler_capi_args()` 从 `HostPayloadRegistry` 取得`Vec<u8>`；
4. `Tensor::new()` 接收该 `Vec` 的 `as_ptr()`/`as_mut_ptr()`；
5. Simpler 以普通 HOST tensor 执行 callable。

P1 已增加独立的 UB_GM materialization 分支。该分支构造 synthetic aperture pointer、
`AddressSpace::UB_GM` tensor 和 dispatch-local binding，不读取 `HostPayloadRegistry`，
Simpler 也不执行 H2D/D2H copy。PTO CPU `TLOAD/TSTORE` 已能通过 mock byte-array
callback 访问这一地址，并在 OOB、权限冲突、callback 失败和 unbind 后访问时
fail-closed。

P2 已把 QEMU access callback 注册到现有 Rust bridge，并通过既有
Simpler/ChipBackend adaptor 把 request-scoped run context 传播到执行 PTO callable 的
worker。`linqu_uapi_kick()` 能识别 tag-10 slot，DMA 读取 control/memref/scalar/shape/
stride table，校验 ABI、CRC、callable fingerprint、requester CNA、layout、bounds、
access 和 opaque OBMM mapping reference，再注册 binding 并进入现有 bridge。
mapping reference 绑定到 EL0 已注册的 OBMM endpoint map；QEMU 先校验 map slot 与
generation，再解析其私有 SIM_DEC route。每次 `TLOAD/TSTORE/fence` callback 都重新
校验 endpoint map 和底层 route；completion、bridge submission failure 和 doorbell
failure 会执行 unbind。

P3 已补齐 guest tag-10 control table、opaque map reference、producer/consumer workload
与统一 acceptance runner。2026-08-30 的 r9 在 n4-910c 和 n4-910c1 分别从精确
`ub_sim@53fd476a`、`QEMU@3f54db3b` worktree 完整重建并通过两节点 acceptance：

- Node A 从原始 export mapping 验证 16,384 个 `u32` 输出；
- Node B 沿既有 bridge 执行 callable 1，日志包含两次 65,536-byte load、一次
  65,536-byte store 和一次 fence；
- completion unbind 记录 `load_bytes=131072`、`store_bytes=65536`、`fences=1`、
  `segment_payload_staging_bytes=0`；
- 两台机器的默认日志均没有 `UB_NPU: created`、`SIM_DEC: GVA_MAP`、
  `GVA_S3_MAP`、`GVA_ROUTE_DUMP`、`GSVA_MODE` 或 `GSVA_`；
- 两份 `validation.status` 均为 `pass`，`runner_exit_code=0`，结束后没有 QEMU
  进程残留。

这组证据证明了同步 OBMM authorization 路径的 PTO direct access。P2 随后增加
可恢复 authorization 正向路径：QEMU 在显式 delay 触发 pending 时冻结当前 CMDQ slot、
control/memref/scalar/shape/stride metadata、memref cursor 和 monotonic sequence，保持
CMDQ head 不变；timer completion 到达后重新校验 mapping identity，从保存的 cursor
继续。全部 memref 获权后才注册 binding 并进入 bridge。默认 delay 为零，原同步路径
不增加 pending/resume 事件。

2026-08-30 的 P2 r2 在 n4-910c 和 n4-910c1 使用相同源内容分别完整构建 QEMU，
对三个 memref 各注入 1,000,000 ns authorization delay。两台机器均观测到 3 次
pending、3 次 ready resume 和 3 次 `pending_head=0 tail=1`；sequence 固定为
`0:1,1:2,2:3`，全部授权完成后 head 才推进到 1。后续两次 load、一次 store、一次
fence、producer original-mapping verify 和零 payload staging 全部通过。reset、cancel、
timeout 的系统化负向验证与并发矩阵归入 P4。

### 3.4 Experimental `sim_npu` / `NPU_OP_VECTOR_ADD_U32` 的真实位置

[`ub_npu.c`](../../vendor/qemu_8.2.0_ub/hw/ub/ub_npu.c) 中的`ub_npu_op_vector_add_u32()` 执行以下操作：

1. 校验三个 GSVA descriptor；
2. 对输入执行 read acquire；
3. 用 `g_malloc()` 分配 `a`、`b`、`c`；
4. 调用 `ubc_gsva_device_read()` 读入 `a` 和 `b`；
5. 在 QEMU C 循环中执行 `c[i] = a[i] + b[i]`；
6. 对 output 执行 write acquire；
7. 调用 `ubc_gsva_device_write()` 和 `ubc_gsva_device_fence()`。

这条路径没有调用 `linqu_ub_bridge_*`、`sim-uapi`、`sim-runtime`、`sim-chipbackend-simpler` 或 PTO ISA。它可以作为以下机制的独立 regression oracle：

- GSVA descriptor validation；
- device CNA；
- read/write acquire；
- GSVA device read/write/fence；
- pending coherence 状态处理。

它不承担 PTO dispatch 入口职责。`sim_npu` 及其 GVA/GSVA data path 统一属于 experimental optional features，默认 feature set 不启用。

### 3.5 当前能力与缺口

| 能力 | 当前状态 | 目标改动 |
| --- | --- | --- |
| guest → QEMU UBC → Rust bridge | 已实现 | 保留 |
| `IoOpcode::Dispatch` → ChipBackend | 已实现 | 增加 v2 control metadata |
| Simpler/PTO callable execution | 已实现 | 保留 |
| `host_vector` payload | host `Vec` staging | 增加 UB GM 参数分支 |
| OBMM export/import 与共享内存 backing | 已实现 | 用作 `lingqu_shmem_memref` backing |
| experimental GVA/GSVA mapping | 已实现部分实验能力 | 可选 adaptor；默认路径不依赖 |
| experimental `sim_npu` GSVA read/write/fence | 已实现 | 保留独立 regression；不接入默认 PTO ingress |
| `lingqu_shmem_memref` | ABI/type、runtime view、guest materialization 与 QEMU parser 已实现并通过两节点 E2E | P4 扩展负向/lifetime 矩阵 |
| `AddressSpace::UB_GM` | C++/Rust 同值 `2`、Simpler pass-through、worker run context 与真实 guest E2E 已实现 | P4 扩展复杂 layout |
| PTO CPU UB GM hook | P1 已实现 contiguous ND、tail、range callback 与 fail-closed | P4 扩展复杂 layout |
| QEMU UBC access callback | P2 已实现 read/write/fence、逐次 mapping 重校验和 completion cleanup；P3 同步 E2E 通过 | P4 失败矩阵与并发 |
| backend authorization 后进入 bridge | P2 已实现同步 fast path 与 pending slot snapshot/resume 正向路径 | P4 补 timeout、reset、cancel 和并发负向矩阵 |
| 两节点 PTO direct E2E | n4-910c、n4-910c1 r9 均通过，默认路径无 NPU/GVA/GSVA 泄漏 | P4 扩展负向、layout 与并发 coverage |

## 4. 修正后的目标架构

### 4.1 控制与执行路径

目标继续使用现有路径：

| 顺序 | 模块 | 目标职责 |
| ---: | --- | --- |
| 1 | guest application | 创建 `lingqu_shmem_memref`，提交 `IoOpcode::Dispatch` v2 control block |
| 2 | QEMU UBC `linqu_uapi` | DMA 读取 control table，解析并授权 UB GM mapping |
| 3 | `sim-qemu` bridge | 注册已授权的 dispatch-local binding，提交原有 dispatch descriptor |
| 4 | `sim-uapi` | 解析 callable 与 memref metadata，禁止读取 payload segment |
| 5 | `sim-runtime` | 构造 UB GM runtime args，管理 dispatch/completion |
| 6 | `sim-chipbackend-simpler` | 构造 `ChipTensor(AddressSpace::UB_GM)`，绑定 callback |
| 7 | Simpler/PTO | 执行真实 callable 和 `TLOAD/TSTORE` |
| 8 | QEMU UB GM interface | 将 binding + UB GM address 转换为 backend load/store/fence |
| 9 | existing CQ path | 经 `sim-qemu` poll 与 QEMU `linqu_uapi_flush_cq()` 返回 guest |

### 4.2 数据路径

目标数据循环为：

`PTO TLOAD/TSTORE` → `PtoSimUbGmAccessOpsV1` → QEMU UB GM memory implementation → `lingqu_shmem` backing → callback result。

QEMU 内部负责 mapping resolution、access、lifetime、ordering 和 requester identity。
Rust bridge 负责 dispatch 与 binding 生命周期，不保存 payload 镜像。experimental GSVA adaptor 可以在内部实现 `PtoSimUbGmAccessOpsV1`，默认路径不引用 GSVA 类型、状态机或 counters。

### 4.3 为什么复用 `IoOpcode::Dispatch`

现有 dispatch 已经覆盖以下能力：

- guest ring/CQ；
- QEMU UBC ingress；
- Rust FFI；
- `sim-uapi` dispatch routing；
- `sim-runtime` lifecycle；
- Simpler artifact/callable execution；
- completion 返回。

新增 attached-NPU opcode 会形成第二套 command、completion、callable registry 和错误模型，也会绕开已经验证的 ChipBackend route。复用现有 dispatch 能把新增范围收敛到 memref metadata、backend authorization、UB GM binding 和 PTO load/store hook。

## 5. 公共编程模型

### 5.1 `lingqu_shmem_memref`

公共 API 使用 opaque `lingqu_shmem_memref`，至少表达：

- 所属 `lingqu_shmem` region/import；
- byte offset 和 length；
- dtype；
- rank、shape 和 stride；
- READ、WRITE 或 READ_WRITE 权限；
- 对 imported mapping 的强生命周期引用。

示例接口属于待冻结草案：

```cpp
lingqu_shmem_region region =
    lingqu_shmem_import(export_handle);

lingqu_shmem_memref input_a =
    lingqu_shmem_memref_create(
        &region,
        input_a_offset,
        input_bytes,
        LINGQU_DTYPE_F32,
        input_shape,
        input_strides,
        LINGQU_SHMEM_ACCESS_READ);

lingqu_shmem_memref input_b =
    lingqu_shmem_memref_create(
        &region,
        input_b_offset,
        input_bytes,
        LINGQU_DTYPE_F32,
        input_shape,
        input_strides,
        LINGQU_SHMEM_ACCESS_READ);

lingqu_shmem_memref output =
    lingqu_shmem_memref_create(
        &region,
        output_offset,
        output_bytes,
        LINGQU_DTYPE_F32,
        output_shape,
        output_strides,
        LINGQU_SHMEM_ACCESS_WRITE);

lingqu_pto_dispatch_builder dispatch =
    lingqu_pto_dispatch_create(vector_add_program_id);

lingqu_pto_dispatch_add_memref(&dispatch, "input_a", &input_a);
lingqu_pto_dispatch_add_memref(&dispatch, "input_b", &input_b);
lingqu_pto_dispatch_add_memref(&dispatch, "output", &output);
lingqu_pto_dispatch_add_u64(&dispatch, "count", element_count);
lingqu_pto_dispatch_submit(&dispatch);
```

公共对象不暴露 QEMU host pointer，也不包含 GVA/GSVA key、token 或 epoch。
版本化 control table 只传递 opaque mapping reference、UB GM address/view metadata、access 和 callable metadata；QEMU adaptor 自行解析其私有 mapping state。

simulator adaptor 当前把一个有效的 `obmm_async_map` slot 与 generation 编码成
64-bit mapping reference。编码仅用于 guest/QEMU 的 simulator wire ABI；
`lingqu_shmem_memref` API 返回 opaque 值，应用不读取 slot 或 generation。QEMU 必须
在提交时和每次 PTO access 时验证该 reference 仍对应同一个活动 OBMM map，并核对其
底层 route identity、range、token、peer CNA 和权限。map unregister 或 generation
变化会使旧 reference 立即失效。

### 5.2 PTO kernel 与 `UB_GM` 参数

#### 5.2.1 kernel 源码中的 `__gm__`

PTO kernel 的参数继续写成 `__gm__ T *`。这里的 `__gm__` 表达 PTO ISA 的
global-memory operand 类别，不指定 simulator backing 来自 HOST、DEVICE 还是
`UB_GM`。`AddressSpace::UB_GM` 是 dispatch 时附着在 `ChipTensor` 上的运行时属性，
不会引入 `__ub_gm__` 之类的新 kernel qualifier。

下面的 kernel 可以用于普通 GM 和 `UB_GM`。每个 tile 通过偏移后的 GM pointer
构造 `GlobalTensor`；`TLOAD/TSTORE` 是唯一允许触碰 global payload 的指令：

```cpp
__aicore__ void vector_add(
    __gm__ float *input_a,
    __gm__ float *input_b,
    __gm__ float *output,
    uint32_t count)
{
    using Global = pto::GlobalTensor<
        float,
        VectorShape,
        VectorStride,
        pto::Layout::ND>;

    InputTile ta;
    InputTile tb;
    OutputTile tout;

    for (uint32_t offset = 0; offset < count; offset += TILE_ELEMENTS) {
        Global ga(input_a + offset);
        Global gb(input_b + offset);
        Global gout(output + offset);

        TLOAD(ta, ga);
        TLOAD(tb, gb);
        TADD(tout, ta, tb);
        TSTORE(gout, tout);
    }
}
```

kernel source 看不到 `AddressSpace::UB_GM` 是有意设计。这样，同一份 PTO binary
可以由普通 GM 参数调用，也可以由 `lingqu_shmem_memref` materialize 出来的
`UB_GM` 参数调用。两条路径的差别发生在 callable 参数构造和 PTO CPU memory
access dispatch 中。

#### 5.2.2 普通 GM 参数与 `UB_GM` 参数的调用差异

| 层次 | 普通 GM 参数 | `UB_GM` 参数 |
| --- | --- | --- |
| PTO kernel signature | `__gm__ float *` | `__gm__ float *` |
| `ChipTensor.address_space` | `AddressSpace::HOST` 或 `DEVICE` | `AddressSpace::UB_GM` |
| `ChipTensor.buffer.addr` | CPU 可解引用 pointer 或 device address | 当前 dispatch 内的 synthetic aperture address |
| dispatch binding | 不需要 UB GM binding | 必须存在 request-scoped `PtoSimUbGmBindingV1` |
| `TLOAD` | 现有 CPU/device GM load | binding lookup → QEMU UB GM `read` callback |
| `TSTORE` | 现有 CPU/device GM store | binding lookup → QEMU UB GM `write` callback |
| payload staging | 当前 `host_vector` 会进入 `HostPayloadRegistry` | 禁止进入 `segment_payloads` 和 `HostPayloadRegistry` |

下面是目标内部实现的等价伪代码，用于展示一个 `lingqu_shmem_memref` 如何成为
真正的 `UB_GM` callable 参数。函数名将在 P0/P1 冻结；这段代码不表示当前仓库已经
提供这些 API：

```cpp
// QEMU 已完成 mapping、bounds、access 和 lifetime authorization。
QemuResolvedUbGmView input_a_resolved =
    qemu_resolve_ub_gm_memref(input_a.opaque_mapping_ref, input_a.byte_offset);
QemuResolvedUbGmView output_resolved =
    qemu_resolve_ub_gm_memref(output.opaque_mapping_ref, output.byte_offset);

PtoSimUbGmBindingV1 input_a_binding = {
    .request_id = request_id,
    .binding_id = 41,
    .aperture_base = 0x700000000000,
    .aperture_length = input_bytes,
    .ub_gm_base = input_a_resolved.ub_gm_addr,
    .mapped_length = input_bytes,
    .access = PTO_SIM_UB_GM_READ,
    .backend_cookie = input_a_resolved.backend_cookie,
};

PtoSimUbGmBindingV1 output_binding = {
    .request_id = request_id,
    .binding_id = 43,
    .aperture_base = 0x700000020000,
    .aperture_length = output_bytes,
    .ub_gm_base = output_resolved.ub_gm_addr,
    .mapped_length = output_bytes,
    .access = PTO_SIM_UB_GM_WRITE,
    .backend_cookie = output_resolved.backend_cookie,
};

ChipTensor input_a_arg = make_tensor_external(
    reinterpret_cast<void *>(input_a_binding.aperture_base),
    input_shape,
    input_rank,
    DataType::FLOAT32,
    false,
    0,
    AddressSpace::UB_GM);

ChipTensor output_arg = make_tensor_external(
    reinterpret_cast<void *>(output_binding.aperture_base),
    output_shape,
    output_rank,
    DataType::FLOAT32,
    false,
    0,
    AddressSpace::UB_GM);
```

`input_b` 按相同方式生成 READ binding。`ChipStorageTaskArgs` 最终携带三个
`ChipTensor` 和 `count` scalar。这里最关键的两个字段是：

- `address_space = UB_GM`：要求 Simpler 跳过 H2D/D2H、拒绝 HOST pointer 路径；
- `buffer.addr = aperture_base`：kernel 接收到的 `__gm__ float *` 是 synthetic
  address，只用于地址计算，不能由 CPU 直接解引用。

Simpler 展开 callable 参数后，kernel entry 实际观察到的值等价于：

```cpp
// 这些值来自三个 AddressSpace::UB_GM ChipTensor，不是 host allocation。
__gm__ float *input_a = reinterpret_cast<__gm__ float *>(0x700000000000);
__gm__ float *input_b = reinterpret_cast<__gm__ float *>(0x700000010000);
__gm__ float *output  = reinterpret_cast<__gm__ float *>(0x700000020000);

vector_add(input_a, input_b, output, element_count);
```

这段代码展示的是 callable argument unpacking 后的逻辑值，不能在普通应用代码中
直接构造或解引用这些 pointer。只有 bridge 注册过的 binding 和本次 run context
可以解释这些地址。

synthetic aperture 必须来自 simulator 预留且不可读写的虚拟地址区间，长度至少覆盖
整个 tensor view。这样可避免它与真实 host allocation 冲突，意外直接解引用也会立即
失败。PTO CPU resolver 把 pointer value 转成 `uintptr_t` 后执行 checked range
arithmetic；callback 返回前不把该地址交给普通 CPU load/store。

Rust materialization 的目标分支等价于：

```rust
match runtime_arg {
    SimplerRuntimeArg::UbGmMemref { binding, view } => {
        ub_gm_bindings.register(request_id, binding)?;
        tensors.push(Tensor::from_ub_gm(
            binding.aperture_base,
            binding.aperture_length,
            view.shape,
            view.strides,
            view.dtype,
        )?);
    }
    SimplerRuntimeArg::InputSegment { .. } => {
        // 现有 staged HOST 路径。
    }
    _ => { /* output, inout, scalar */ }
}
```

目标实现需要新增 `Tensor::from_ub_gm()` 或等价的显式构造函数。现有
`Tensor::new()` 固定生成 HOST tensor，不能用于 `UB_GM` 参数。

#### 5.2.3 `UB_GM` 在实际执行时如何生效

单独增加 `AddressSpace::UB_GM = 2` 不足以改变 `TLOAD/TSTORE` 行为。完整生效链
包含以下步骤：

| 步骤 | 组件 | 生效动作 |
| ---: | --- | --- |
| 1 | guest/QEMU UBC | 从 dispatch control table 读取 `lingqu_shmem_memref`，校验 mapping、view、bounds、access 和 lifetime |
| 2 | QEMU UBC | 在进入同步 PTO dispatch 前完成 backend authorization，并为每个 memref 建立 `PtoSimUbGmBindingV1` |
| 3 | `sim-qemu` bridge | 把 binding metadata 绑定到 `request_id`；只传 metadata，不复制 payload |
| 4 | `sim-uapi` / `sim-runtime` | 生成 `SimplerRuntimeArg::UbGmMemref`，跳过 `segment_payloads` 和 `HostPayloadRegistry` |
| 5 | `sim-chipbackend-simpler` | 构造 `address_space=UB_GM`、`buffer.addr=aperture_base` 的 `ChipTensor` |
| 6 | Simpler CPU run context | 把当前 `request_id`、binding registry、callback table 和 QEMU context 关联到本次 run，并传播到执行 callable 的 CPU worker |
| 7 | PTO kernel | `GlobalTensor` 保存 synthetic pointer；普通 C++ pointer arithmetic 只改变 aperture 内的地址值 |
| 8 | PTO CPU `TLOAD/TSTORE` | 在任何 pointer dereference 之前用 current run context 查询 binding；命中后改走 UB GM read/write callback |
| 9 | QEMU UB GM implementation | 把 `ub_gm_addr` 解析到 `lingqu_shmem` 原始 backing，执行 read/write；output 在完成前执行 fence |
| 10 | completion | 释放 bindings，经原有 CQ 返回；producer 从原始 mapping 读取 PTO 写入结果 |

这条链包含三个用途不同的生效机制：

1. `ChipTensor.address_space = UB_GM` 控制参数 materialization 和 copy policy；
2. `buffer.addr = aperture_base` 为每条 PTO memory instruction 提供 binding lookup key；
3. per-run context 把 lookup 结果关联到本次 `request_id`、QEMU callback 和 backing。

第一项不会独自触发 callback，第二项也不能脱离第三项解析。三项同时成立时，UB GM
参数才具有完整执行语义。

Simpler 当前若用 `is_device_memory()` 判断是否跳过 H2D/D2H，那么仅添加枚举值仍会把
`UB_GM` 当成 HOST。目标实现需要把 copy decision 改为完整的 `AddressSpace` 分派：

```cpp
switch (tensor.address_space) {
case AddressSpace::HOST:
    prepare_host_transfer(tensor);
    break;
case AddressSpace::DEVICE:
    use_child_device_allocation(tensor);
    break;
case AddressSpace::UB_GM:
    require_dispatch_binding(tensor);
    skip_h2d_and_d2h(tensor);
    break;
default:
    fail("unsupported_address_space");
}
```

第 6 步需要一个 per-run context。其逻辑内容如下：

```cpp
struct PtoSimUbGmRunContextV1 {
    uint64_t request_id;
    const PtoSimUbGmBindingRegistryV1 *bindings;
    const PtoSimUbGmAccessOpsV1 *access_ops;
    void *qemu_context;
};
```

该 context 必须随 Simpler run 传播到所有执行该 callable 的 CPU worker。禁止使用一个
无 request identity 的进程级可变 singleton，否则并发 dispatch 可能解析到其他请求的
binding。

#### 5.2.4 一个 `TLOAD` 的地址解析实例

假设一个 READ binding 为：

```text
request_id       = 9001
binding_id       = 41
aperture_base    = 0x7000_0000_0000
aperture_length  = 4096
ub_gm_base       = 0x0020_0000
```

kernel 执行到第 64 个 `float` 时，`input_a + 64` 产生 synthetic address
`0x7000_0000_0100`。`TLOAD` 请求读取 256 bytes 时，resolver 执行：

```text
relative_offset = 0x7000_0000_0100 - 0x7000_0000_0000 = 0x100
end             = 0x100 + 0x100 = 0x200 <= aperture_length
ub_gm_addr      = 0x0020_0000 + 0x100 = 0x0020_0100
```

随后调用：

```c
access_ops->read(
    qemu_context,
    9001,
    41,
    0x00200100,
    tile_scratch,
    256);
```

QEMU 把 `lingqu_shmem` backing 的 256 bytes 写入 `tile_scratch`，现有 PTO layout
逻辑再把 scratch materialize 成 `InputTile`。`TSTORE` 执行逆向过程：先把
`OutputTile` 转换为连续 fragments，再对每个 fragment 调用 `write`，最后对 dirty
output binding 调用 `fence`。

因此，PTO kernel 中看到的仍是 `GlobalTensor` 和 `TLOAD/TSTORE`；实际 payload
访问已经由 synthetic address + current run context 转入 QEMU UB GM callback。

#### 5.2.5 fail-closed 条件与当前实现状态

resolver 必须按以下顺序处理地址：

1. 地址完整落入当前 request 的 UB GM binding：执行 callback；
2. 地址落入保留的 synthetic aperture 区域，但当前 request 没有 matching binding：
   返回 `pto_ub_gm_unbound`，禁止直接解引用；
3. 地址属于 HOST/DEVICE 参数：进入对应的现有实现；
4. range 跨 binding、越界、权限不符、callback 缺失或 lifetime 已失效：失败并清理
   当前 dispatch。

P1 已实现 synthetic aperture 到 PTO callback 的 mock/CPU 生效链。对应代码证据为：

| 文件 | 当前行为 |
| --- | --- |
| `vendor/simpler/src/common/task_interface/data_type.h` | `AddressSpace` 定义 `HOST = 0`、`DEVICE = 1`、`UB_GM = 2` |
| `crates/sim-chipbackend-simpler/src/lib.rs` | `Tensor::from_ub_gm()` 构造保留区 pointer、shape、stride、dtype 和 `UB_GM` descriptor |
| `crates/sim-runtime/src/lib.rs` | UB_GM arg 校验 request/binding/access/bounds 后 materialize；该分支不访问 `HostPayloadRegistry` |
| `vendor/simpler` HBG/TMRB runtime | HOST 沿用 copy；DEVICE/UB_GM 直接传递 descriptor pointer，不做 H2D/D2H |
| `vendor/pto-isa/include/pto/cpu/ub_gm_access.hpp` | 从当前 run context 解析 binding、权限、range 与 callback，保留区未绑定时 fail-closed |
| `vendor/pto-isa/include/pto/cpu/TLoad.hpp` | 在 pointer dereference 前识别 synthetic address；contiguous ND 按 range read 到 scratch |
| `vendor/pto-isa/include/pto/cpu/TStore.hpp` | 在 pointer dereference 前识别 synthetic address；contiguous ND 从 scratch 按 range write |

P1 测试使用 mock byte-array backend 运行真实 PTO `TLOAD → TADD → TSTORE`，覆盖
64×64、3×5 tail tile、OOB、read-only output、callback failure 和 unbind-after-use。
ASan 下 6 个 UB_GM 用例全部通过；普通 HOST 64×64 vector-add 回归也通过。

P2 已实现真实 QEMU callback 注册、worker-local run context、同步 OBMM
authorization 和 completion cleanup。callback 缺失、binding 过期、mapping
generation 改变、range 越界或权限冲突时继续 fail-closed，不会回退到 host staging
或直接解引用 synthetic pointer。P3 guest 已提交真实 tag-10 descriptor；n4-910c 和
n4-910c1 r9 两节点 runtime acceptance 均通过，并在同一个 request 中观察到
`TLOAD/TSTORE` 对应的 QEMU UB GM load/store/fence callback。

## 6. ABI 与内部数据结构

### 6.1 `AddressSpace::UB_GM`

Simpler C++ ABI 增加：

```cpp
enum class AddressSpace : uint8_t {
    HOST = 0,
    DEVICE = 1,
    UB_GM = 2,
};
```

Rust mirror 增加同值枚举：

```rust
#[repr(u8)]
pub enum AddressSpace {
    Host = 0,
    Device = 1,
    UbGm = 2,
}
```

`ChipTensor` 已经包含 1-byte `address_space` 和预留 padding。P0 必须用 C++ `static_assert` 和 Rust layout test 证明 `sizeof(ChipTensor)` 仍为 128 bytes。

`UB_GM` tensor 的规则：

- 禁止 H2D 和 D2H；
- `buffer.addr` 只存 synthetic aperture address，宿主代码禁止直接解引用；
- tensor 全范围必须落入当前 dispatch 的 binding；
- dtype、shape、stride 和 view offset 推导的最大 extent 必须在 bounds 内；
- `TSTORE` 要求 WRITE，`TLOAD` 要求 READ；
- binding 释放、mapping 失效、lifetime 过期或 callback 缺失时 dispatch 失败；
- 禁止隐式转换成 HOST tensor。

### 6.2 dispatch-local synthetic aperture

PTO CPU `GlobalTensor` 当前通过 typed pointer 做 address arithmetic。V1 为每个 UB GM memref 分配不可解引用的 synthetic aperture；registry 保存 aperture 与 opaque UB GM mapping 的对应关系：

```cpp
struct PtoSimUbGmBindingV1 {
    uint64_t request_id;
    uint64_t binding_id;
    uint64_t aperture_base;
    uint64_t aperture_length;
    uint64_t ub_gm_base;
    uint64_t mapped_length;
    uint32_t access;
    uint32_t flags;
    uint64_t backend_cookie;
};
```

registry 的作用域是一次 dispatch。正常完成、异常、取消和 QEMU reset 都必须执行 unbind。不同 dispatch 的 `binding_id` 不能交叉解析。

`backend_cookie` 由 QEMU adaptor 解释。默认 ABI 不定义其内容；experimental GSVA adaptor 可以在 QEMU 私有表中把它解析为 GSVA key/token/epoch，相关结构不得进入 PTO、Simpler、`lingqu_shmem_memref` 或默认 dispatch control table。

### 6.3 QEMU UBC access callback

callback table 只属于 simulator 内部 ABI：

```c
typedef struct PtoSimUbGmAccessOpsV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;

    int (*read)(
        void *qemu_context,
        uint64_t request_id,
        uint64_t binding_id,
        uint64_t ub_gm_addr,
        void *dst,
        uint64_t length);

    int (*write)(
        void *qemu_context,
        uint64_t request_id,
        uint64_t binding_id,
        uint64_t ub_gm_addr,
        const void *src,
        uint64_t length);

    int (*fence)(
        void *qemu_context,
        uint64_t request_id,
        uint64_t binding_id,
        uint64_t ub_gm_addr,
        uint64_t length,
        uint32_t flags);
} PtoSimUbGmAccessOpsV1;
```

现有 bridge 新增注册接口，保留已有 new/register/submit/ring/poll 接口：

```c
int linqu_ub_bridge_register_ub_gm_access_v1(
    LinquUbBridge *bridge,
    const PtoSimUbGmAccessOpsV1 *ops,
    void *qemu_context,
    uint32_t pto_device_cna);
```

QEMU 在 `linqu_uapi_init_bridge()` 成功后注册 callback 和 device CNA。默认 callback 接收 `binding_id + UB GM address + length`，解析 opaque mapping，再执行 simulator load/store/fence。

experimental GSVA adaptor 可以在其私有实现中调用 `ubc_gsva_device_read/write/fence()`。这些函数不进入默认 callback ABI；callback 也不在同步 PTO dispatch 内等待实验性 GSVA acquire。

### 6.4 `IoOpcode::Dispatch` v2 control table

现有 ring slot 为 64 bytes，不适合内嵌任意数量的 memref。P0 增加 versioned dispatch control table；ring slot 继续表达 `IoOpcode::Dispatch` 和 control table reference。

P0 将最终 byte layout 固定在
`crates/sim-qemu/include/linqu_shmem_pto_abi.h`。C/C++ 和 Rust mirror 使用下列
尺寸：

| ABI 对象 | 固定尺寸 | 用途 |
| --- | ---: | --- |
| `LingquPtoDispatchSlotV2` | 64 bytes | ring 中现有 Dispatch v2 的 control-table reference |
| `LingquPtoDispatchControlV2` | 64 bytes | ring slot 引用的 dispatch control |
| `LingquShmemMemrefV1` | 80 bytes | 一个 `lingqu_shmem_memref` wire view |
| `LingquPtoScalarV1` | 24 bytes | 一个 scalar 参数 |
| `PtoSimUbGmBindingV1` | 64 bytes | QEMU 授权后的 dispatch-local binding |
| `PtoSimUbGmAccessOpsV1` | 32 bytes | QEMU UBC callback table |
| `LingquPtoUbGmCountersV1` | 96 bytes | no-staging 和 byte accounting |

control 的精确字段为：

```c
struct LingquPtoDispatchControlV2 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    uint64_t request_id;
    uint64_t callable_id;
    uint32_t memref_count;
    uint32_t scalar_count;
    uint64_t memref_table_iova;
    uint64_t scalar_table_iova;
    uint64_t artifact_fingerprint;
    uint32_t metadata_crc32;
    uint32_t requester_cna;
};
```

ring slot 使用 transport tag `10` 表示现有 `IoOpcode::Dispatch` 的 v2
control-table 形式。byte 0 为 tag，byte 1..8 为 little-endian `op_id`，byte
9..16 为 little-endian `control_table_iova`，byte 17..63 必须为零。该 tag 只区分
64-byte descriptor 的 wire layout，不增加 NPU opcode，也不改变 dispatch 的
ChipBackend/Simpler/PTO 生命周期。精确布局由
`LingquPtoDispatchSlotV2`、`LINGQU_PTO_DISPATCH_SLOT_OP_ID_OFFSET` 和
`LINGQU_PTO_DISPATCH_SLOT_CONTROL_IOVA_OFFSET` 固定。

`metadata_crc32` 使用 IEEE CRC-32。计算时先把 control 中该字段清零，然后按 wire
顺序拼接 control、memref table、scalar table，再按 memref table 顺序拼接各 memref
的 `rank * sizeof(uint32_t)` shape 和 stride table。合法 CRC 值可以为零，QEMU 必须
计算后比较，不能把非零检查当作 CRC 校验。

每个 80-byte memref entry 固定包含 ABI version/size、opaque mapping reference、
UB GM address、byte offset/length、shape/stride table IOVA、argument index、rank、
dtype、role、access、flags 和两个必须为零的 reserved fields。QEMU DMA 读取 control
table，完成 bounds、version、count、CRC、mapping 和权限校验。QEMU 只把经过授权的
binding metadata 注册给 Rust bridge；payload bytes 留在 `lingqu_shmem` backing。

Rust 内部可以增加 `UapiDescriptor::DispatchUbGmV2`，但它必须进入现有 `run_chipbackend_dispatch()`/ChipBackend lifecycle。该类型表示现有 semantic opcode 的版本化参数，不形成新的 NPU command family。

## 7. 安全执行状态机

某些 QEMU UB GM memory implementations 可能需要异步 mapping authorization，而
Simpler/PTO C API dispatch 当前同步执行。P2 已把可恢复 preflight 状态机实现到
QEMU UBC：任何可能 pending 的 backend authorization 都在调用
`linqu_ub_bridge_ring_doorbell()` 之前完成。默认状态机不规定 GVA/GSVA 协议；
experimental GSVA adaptor 可以在内部把 authorization 实现为 acquire/ACK。

![IoOpcode::Dispatch v2 的 UB_GM 安全执行状态机](2026-08-29-lingqu-shmem-pto-ub-gm-command-state.svg)

当前已实现顺序：

1. QEMU 读取 64-byte ring slot 和 versioned control table；
2. 校验 callable、artifact fingerprint、memref count、shape/stride extent、mapping、bounds 和 access；
3. 使用 PTO/ChipBackend device CNA 授权全部 input range；
4. 授权全部 output/inout range；
5. backend authorization pending 时冻结 slot、control/memref/scalar/shape/stride
   snapshot、memref cursor 和 monotonic sequence，保持 CMDQ head 不变；
6. backend completion 到达后重新校验 mapping identity，并从保存的 cursor 重试；
7. 全部 range 授权成功后，在 bridge 注册 dispatch-local bindings；
8. 提交原有 `IoOpcode::Dispatch` 并 ring doorbell；
9. `sim-uapi → sim-runtime → sim-chipbackend-simpler → Simpler/PTO` 同步执行；
10. `TLOAD/TSTORE` callback 只进行已获权 range 的同步 read/write；
11. 对 dirty output fence，unbind，再经已有 CQ path 返回 completion。

`pto-authorization-delay-ns=0` 为默认同步 fast path。正值用于确定性触发 pending/
resume validation，并不把 timer delay 定义成唯一 provider 协议。若在同步 PTO callback
内等待 backend authorization，QEMU event loop 可能正被同一 callback 占用，进而形成
死锁。当前实现只在 bridge dispatch 前暂停和恢复 authorization；进入 PTO callback
后不等待 authorization。

## 8. PTO `TLOAD/TSTORE` 的 CPU 仿真实现

### 8.1 address-space 分派

`ChipTensor.address_space` 在 callable materialization 阶段决定 copy policy 和参数构造。
进入 kernel 后，当前 `GlobalTensor` 只保留 pointer、shape 和 stride，没有保存
`ChipTensor.address_space`。因此，PTO CPU load/store 必须在进入现有 layout
implementation 和任何 pointer dereference 前，通过 synthetic address 与 current run
context 完成分派。

示意逻辑如下：

```cpp
PtoSimUbGmRunContextV1 *run = pto_sim_current_run_context();
PtoSimUbGmResolution resolution = pto_sim_resolve_ub_gm(
    run,
    reinterpret_cast<uint64_t>(global_address),
    requested_ranges,
    Access::READ);

if (resolution.matched) {
    pto_sim_ub_gm_tload(run, resolution, tile, requested_ranges);
    return;
}

if (pto_sim_is_reserved_ub_gm_aperture(global_address, requested_ranges)) {
    fail("pto_ub_gm_unbound");
}

cpu_local_tload(tile, global_address, valid_region);
```

`TSTORE` 使用相同 resolver，并把 access 改为 WRITE。UB GM range lookup、bounds、
access 或 lifetime 检查失败时返回确定错误；地址只要落入 synthetic aperture 保留区，
代码就禁止继续走普通 pointer dereference。

P1 已在 `TLOAD_TILE_IMPL()` 和 `TSTORE_IMPL()` 的公共入口、任何 pointer dereference
之前增加保留区检查。普通 HOST/DEVICE 地址继续进入原实现；synthetic UB_GM 地址先
解析当前 run context。V1 支持 contiguous ND 和 tail tile；synthetic 地址遇到 DN、NZ、
stride 或 atomic store 时返回 `pto_ub_gm_bad_memref`，不会绕回普通 pointer 路径。
P4 再扩展这些 layout，而 fail-closed 分派边界从 P1 起保持不变。

### 8.2 callback 粒度

callback 粒度采用 tile 或合并后的连续 fragment：

1. 根据 shape、stride、layout 和 valid region 计算 byte ranges；
2. 合并相邻 ranges；
3. `TLOAD` 按 range 读取到 tile scratch；
4. 复用现有 layout conversion；
5. `TSTORE` 把 tile 转换为连续 fragments；
6. 批量写入并标记 binding dirty；
7. kernel 完成后 fence dirty bindings。

禁止为每个 scalar element 调一次 QEMU callback。

### 8.3 分阶段 layout 覆盖

| 版本 | 支持范围 |
| --- | --- |
| V1 | contiguous ND、自然对齐、tail tile、普通 READ/WRITE |
| V1.1 | cross-page、unaligned offset |
| V1.2 | stride、DN、NZ |
| V1.3 | range coalescing、多个相邻 tensor view |
| V2 | atomic store、重叠写和更复杂的一致性语义 |

## 9. 最小可信两节点 PoC

### 9.1 workload

Node A producer：

1. 创建并 export 一个 `lingqu_shmem` region；
2. 在 region 内划分 A、B 和 Out；
3. 写入 A/B，清零 Out；
4. 发布 export handle 和 view metadata；
5. 等待 Node B completion；
6. 从原始 mapping 验证 `Out[i] == A[i] + B[i]`。

Node B consumer：

1. import Node A 的 region；
2. 创建三个 `lingqu_shmem_memref`；
3. 提交现有 `IoOpcode::Dispatch` 的 v2 control block；
4. QEMU UB GM implementation 完成 input/output mapping authorization；
5. 现有 bridge 路由到 Simpler/PTO vector-add callable；
6. PTO 执行真实 `TLOAD/TADD/TSTORE`；
7. 等待现有 CQ completion；
8. release memrefs 和 import mapping。

### 9.2 关联日志

当前 r9 同步 acceptance 与 P2 r2 pending/resume acceptance 对同一
`op_id`/`request_id` 检查以下实际日志契约：

```text
LINGQU_SHMEM_PTO role=producer stage=published
LINGQU_SHMEM_PTO role=consumer stage=prepared ... mapping_ref=...
QEMU_UB_GM_ACCESS_REGISTER pto_device_cna=...
QEMU_UB_GM_INPUT_AUTHORIZE
QEMU_UB_GM_OUTPUT_AUTHORIZE
QEMU_UB_GM_AUTHORIZATION_PENDING ... slot=... cursor=... sequence=...
QEMU_UB_GM_AUTHORIZATION_RESUME ... slot=... cursor=... sequence=... status=ready
SIM_QEMU_UB_GM_BIND_REGISTER
name=simpler_run.bind
QEMU_UB_GM_LOAD
QEMU_UB_GM_STORE
QEMU_UB_GM_FENCE
QEMU_UB_GM_UNBIND ... segment_payload_staging_bytes=0
LINGQU_SHMEM_PTO role=consumer stage=completion ... completion_status=1
LINGQU_SHMEM_PTO role=producer producer_verify=pass
```

`QEMU_UB_GM_LOAD/STORE/FENCE` 由执行 callable 的 PTO worker callback 产生；紧邻的
`simpler_run.*` spans 证明 callback 位于现有 Simpler/PTO 执行窗口内。acceptance 还会
拒绝 `UB_NPU: created`、GVA mapping/route 和任意 `GSVA_`/`GSVA_MODE` 日志。
默认 delay 为零时，runner 要求 pending/resume 日志数量为零；显式 delay 为正时，
runner 要求每个 memref 恰好出现一次 pending 和一次 ready resume，并检查 CMDQ head
在 pending 窗口内保持不变。

### 9.3 machine-checkable counters

对 16,384 个 `u32` 元素，r9 的 machine-checkable 实际值为：

| 指标 | n4-910c | n4-910c1 | 断言 |
| --- | ---: | ---: | --- |
| input authorization | 2 | 2 | 每个 65,536 bytes |
| output authorization | 1 | 1 | 65,536 bytes |
| QEMU UB GM load | 2 | 2 | 合计 131,072 bytes |
| QEMU UB GM store | 1 | 1 | 合计 65,536 bytes |
| QEMU UB GM fence | 1 | 1 | output range 65,536 bytes |
| `segment_payload_staging_bytes` | 0 | 0 | 必须为 0 |
| producer original-mapping verify | pass | pass | 必须为 pass |
| experimental path leakage | 0 | 0 | 必须为 0 |
| QEMU leftovers | 0 | 0 | 必须为 0 |

验收断言：

```text
h2d_bytes == 0
d2h_bytes == 0
segment_payload_staging_bytes == 0
pto_ub_gm_read_bytes == qemu_ub_gm_load_bytes
pto_ub_gm_write_bytes == qemu_ub_gm_store_bytes
qemu_ub_gm_fence_count >= 1
producer_verify == pass
```

P1 的 Simpler pass-through tests 独立证明 UB_GM tensor 不触发 H2D/D2H；r9 进一步
证明实际 guest dispatch 的 `segment_payload_staging_bytes` 为零。当前结构化 unbind
记录尚未单独输出 `h2d_bytes`/`d2h_bytes` 字段，P5 会把这些路径级 counters 汇总到
同一报告。

### 9.4 r9 双机证据

| 项目 | n4-910c | n4-910c1 |
| --- | --- | --- |
| run id | `pto-ub-gm-p3-e2e-n4-20260830-r9` | `pto-ub-gm-p3-e2e-n4c1-20260830-r9` |
| root revision | `53fd476af2e974cf83648231b1602b987cb82495` | 同左 |
| QEMU revision | `3f54db3bc3c993420b76d69459689d672e2971a9` | 同左 |
| callable fingerprint | `0x46fb4d59b4d3e5ce` | 同左 |
| manifest SHA-256 | `04820fadd8032b31ad193361ff1b398121a9f0ff9b1390f72207f52fbcb6038d` | 同左 |
| Image SHA-256 | `bfde80c070846d146d7360112e14b66457bde3e3423f9798d7f5330410f623f1` | `21c97eaa3b6d7853db325bc3c1238595f49574ec6ab092448997fba752d15fe2` |
| initramfs SHA-256 | `547b3b685fa01b95830dcb2c62c466cbd7d522e5cb320a8fa87566d578b3fa52` | `06571305a976638ac7018080db32edec525e6d4e074f360d343d789c90b5d2c7` |
| QEMU SHA-256 | `56567aa53a4d077081a86caeb05b76e30f6c87fe7e81658eb56fd38c78c033d4` | `16a0f420a18ef47084a8573d18fb955980be816e5bb5db329644c9f5105e63cd` |
| `validation.status` | pass | pass |

完整 evidence 已保存到仓库外的远端 worktree，并复制到本地忽略目录
`out/lingqu-shmem-pto-e2e/<run-id>/`。r8 失败 evidence 保持只读；其唯一失败原因是
默认 `GSVA_MODE arm_mmu` 日志泄漏，数据路径本身已经完成。r9 修正后重新完整构建，
没有覆盖或混用 r8 文件。

当显式启用 experimental GSVA adaptor 时，可以额外输出 `qemu_gsva_device_*` diagnostics；这些字段不进入默认验收契约。

### 9.5 P2 authorization pending/resume 双机证据

P2 在两台机器各运行一组默认同步回归和一组显式 delayed authorization。默认同步
回归证明 delay 为零时原数据路径不产生 pending/resume；下表记录 delayed r2 的正式
通过证据：

| 项目 | n4-910c | n4-910c1 |
| --- | --- | --- |
| run id | `pto-ub-gm-p2-auth-delay-n4-20260830-r2` | `pto-ub-gm-p2-auth-delay-n4c1-20260830-r2` |
| 后续归档的实现 commit | `ub_sim@8ef0b95`、`QEMU@915a7ebe` | 同左 |
| authorization delay / timeout | 1,000,000 ns / 1,000,000,000 ns | 同左 |
| pending / ready resume | 3 / 3 | 3 / 3 |
| pending 时 CMDQ head | 3 次均为 `pending_head=0 tail=1` | 同左 |
| cursor / sequence | `0:1,1:2,2:3` | 同左 |
| 全部授权后的 CMDQ head | `pending_head=1 tail=1` | 同左 |
| load / store / fence | 2 / 1 / 1 | 2 / 1 / 1 |
| `segment_payload_staging_bytes` | 0 | 0 |
| producer verify / experimental leakage | pass / 0 | pass / 0 |
| `validation.status` / QEMU leftovers | pass / 0 | pass / 0 |

两次正式 run 发生在 commit 之前；其 `revisions.txt` 如实记录 root/QEMU baseline 和
working-tree modifications。远程 worktree 的实现源内容随后分别归档为
`ub_sim@8ef0b95` 和 `QEMU@915a7ebe`。这项时间顺序不等同于“在干净 commit checkout
上复跑”；正式证据以保存的 source state、artifact hashes、结构化日志和 runner
结果为准。最初的 delayed r1 数据也保留为诊断证据，它只因旧 leftover detector
把外层 SSH 命令行中的 QEMU 字符串误判为残留而失败；修正后的 r2 使用
`pgrep -af '[q]emu-system-aarch64'` 并在两台机器得到空 leftover 文件。

## 10. 分阶段实施计划

### P0：冻结 ABI、device identity 与验收契约（已完成）

改动：

- 固定 `lingqu_shmem_memref` 语义；
- 固定 `AddressSpace::UB_GM = 2`；
- 定义 `IoOpcode::Dispatch` v2 control table 和 memref table；
- 定义 callback ABI、binding lifetime、error code 和 counters；
- 在 topology/QEMU device 配置中明确 `pto_device_cna`；
- 固定 `sim_npu`、GVA、GSVA 为 experimental optional、default disabled；
- 预留统一 CLI：`lingqu-shmem-pto-e2e`。

退出条件：

- C/C++/Rust layout tests 草案通过；
- malformed v2 metadata 返回确定错误；
- 文档、header 和 contract tests 使用一致字段；
- 新接口中没有 `EXTERNAL_GM`、`external_memref` 或新的 NPU opcode。

预计工作量：2–3 个工程日。

### P1：PTO/Simpler mock `UB_GM`（已完成）

主要改动位置：

- `vendor/simpler/src/common/task_interface/data_type.h`；
- `vendor/simpler/src/common/task_interface/tensor.h`；
- `vendor/pto-isa/include/pto/cpu/TLoad.hpp`；
- `vendor/pto-isa/include/pto/cpu/TStore.hpp`；
- Simpler runtime dispatch-local registry；
- `crates/sim-chipbackend-simpler/src/lib.rs`；
- `crates/sim-runtime/src/lib.rs`。

工作内容：

- 增加 C++/Rust `UB_GM` address-space value；
- 增加 synthetic aperture registry；
- 用 mock byte array callback 运行真实 PTO vector-add callable；
- 为 `prepare_simpler_capi_args()` 增加 UB GM arg 分支；
- 保持 HOST/DEVICE 既有 fast path；
- OOB、read-only write、callback error 和 unbind-after-use fail-closed。

退出条件：

- mock UB GM vector-add 通过；
- `ChipTensor` ABI 仍为 128 bytes；
- callback 以 tile/range 为粒度；
- 既有 HOST/DEVICE vector tests 通过；
- sanitizer 证明 synthetic address 没有被直接解引用。

完成证据：

- `vendor/simpler@70350b51`：HBG/TMRB 的 A2A3/A5 路径均把 UB_GM descriptor
  直接传给 callable， focused pass-through tests 通过；
- `ub_sim@0c52386`：runtime arg/view/binding materialization、zero host staging、
  bounds/access fail-closed unit tests 通过；
- `vendor/pto-isa@594817f2`：真实 PTO CPU `TLOAD → TADD → TSTORE` 在 mock
  UB_GM backend 上通过，6 个 UB_GM tests 与 HOST vector-add regression 通过；
- 同一 6 个 UB_GM tests 在 AddressSanitizer 构建下通过。

这里的“已完成”限定为 P1 mock/CPU 范围。P2 随后接入 QEMU callback、真实 run
context 和同步 OBMM authorization；`lingqu_shmem` 双节点 backing 的运行证据归入
P3。

预计工作量：5–8 个工程日。

### P2：扩展现有 QEMU UBC bridge（仿真正向路径已完成）

主要改动位置：

- QEMU `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c` 与 device-facing header；
- `crates/sim-qemu/include/linqu_ub_bridge.h`；
- `crates/sim-qemu/src/ffi.rs`；
- `crates/sim-qemu/src/types.rs`；
- `crates/sim-qemu/src/adapter.rs`；
- `crates/sim-uapi/src/lib.rs`；
- `crates/sim-runtime/src/lib.rs`；
- `crates/sim-chipbackend-simpler/src/lib.rs`。

工作内容：

- 在 bridge 初始化时注册 `PtoSimUbGmAccessOpsV1` 和 `pto_device_cna`；
- 解析 `IoOpcode::Dispatch` v2 control table；
- 在 `linqu_uapi_kick()` 提交 Rust bridge 前完成 backend mapping authorization；
- authorization pending 时保留当前 slot 并安全恢复；
- 将已校验 binding 注册到 bridge，再进入原有 `submit_slot → ring_doorbell`；
- `sim-uapi` 的 UB GM 分支不读取 `segment_payloads`；
- completion 清理 binding，错误精确映射为现有 `CompletionStatus::FatalFailure { code }`；
- 增加 no-staging 与通用 UB GM byte counters。

P2 不修改 `guest-linux/kernel_ub/include/uapi/ub/ub_npu.h`，不增加 `NPU_OP_PTO_DISPATCH`。

当前实施状态：

- `vendor/qemu_8.2.0_ub@0da2a94a` 已实现 tag-10 decode、CRC/metadata 验证、
  synchronous OBMM mapping authorization、request-scoped binding registry、
  read/write/fence callback 和完成/错误 cleanup；
- `vendor/qemu_8.2.0_ub@dc8d9633` 把 wire mapping reference 绑定到已注册的
  OBMM endpoint map，并在每次 callback 中同时重校验 endpoint generation 和底层
  route identity；
- `vendor/qemu_8.2.0_ub@001eb084` 在 PTO worker callback 期间释放 BQL，避免跨
  QEMU/Rust/Simpler/PTO 同步调用形成 BQL 自锁；
- `vendor/qemu_8.2.0_ub@76942965`、`3f54db3b` 使 NPU、隐式 GSVA route 与
  GSVA ARM-MMU mode 都成为显式 opt-in；
- `vendor/qemu_8.2.0_ub@915a7ebe` 增加 resumable authorization state：冻结
  CMDQ slot 和全部 control metadata，按 memref cursor/sequence 暂停并恢复，
  resume 时重新校验 mapping identity，全部授权后才进入 bridge；
- `ub_sim@8ef0b95` 增加显式 authorization delay/timeout CLI、sync/delayed evidence
  断言、CMDQ head retention gate，并更新 QEMU gitlink；
- QEMU diff-only `checkpatch` 的代码项为 0 error/0 warning；n4-910c 使用
  `guest-linux/aarch64/scripts/build_qemu_binary.sh` 完整构建成功；
- 远程验证工作树为
  `/home/ll/pto_ub_gm_p2_bridge_20260830-r1`；五份 QEMU 候选源文件的 SHA-256 与
  本地内容一致，生成的 Arm64 `qemu-system-aarch64` SHA-256 为
  `57bb74b667de6cb38ab9a28f3890941e8f2572a8e9db8c45714698e78d9b53e3`；
- `sim-qemu` 35 个 unit tests、8 个 ABI/QEMU contract tests，以及远程 QEMU
  `obmm-remote` 6 个、`obmm-remote-model` 7 个、`async-load` 9 个 unit cases 通过；
- 同步 OBMM authorization、guest producer/consumer 和双节点 E2E 已完成；
- n4-910c 与 n4-910c1 的默认同步回归均通过且 pending/resume 计数为零；
- n4-910c 与 n4-910c1 的 delayed r2 均通过：每台 3 次 pending、3 次 ready
  resume，pending 窗口内 CMDQ head 保持 0，全部授权后推进到 1；
- PTO callback 内没有 authorization wait，数据路径保持 2 load、1 store、1 fence、
  zero staging 和 producer verify pass。

退出条件：

- 单节点 local UB GM + existing bridge + PTO vector-add 通过；
- trace 同时包含 bridge dispatch 和真实 PTO `TLOAD/TSTORE`；
- requester 为配置的 PTO device CNA；
- pending authorization 能退出并恢复；
- 同步 callback 内不等待 backend authorization；
- `segment_payload_staging_bytes == 0`。

上述退出条件已经满足。P2 的“仿真正向路径已完成”覆盖同步 fast path，以及由显式
QEMU delay 确定性触发的可恢复 authorization 正向路径。真实异步 provider 将来仍需
接入相同状态机；timeout、reset、cancel、重复 completion 与多 dispatch 竞争的
fail-closed 验证归入 P4。

预计工作量：5–8 个工程日。

### P3：`lingqu_shmem_memref` 与双节点 OBMM E2E（同步路径已完成）

主要改动位置：

- guest Lingqu/OBMM adaptor；
- `guest-linux/aarch64/common/obmm_common.h`；
- 新 guest app `guest-linux/aarch64/apps/lingqu_shmem_pto_direct/`；
- guest artifact/initramfs wiring；
- `sim-cli` 的 `lingqu-shmem-pto-e2e` 子命令；
- Python contract 和远端 QEMU E2E tests。

工作内容：

- 创建 opaque `lingqu_shmem_memref`；
- 实现 region/view/in-flight dispatch lifetime；
- materialize v2 memref table；
- 实现 Node A producer 与 Node B consumer；
- 汇总结构化日志、counters 和 artifact fingerprints；
- 失败后清理 guest、QEMU、mapping 和临时 evidence。

当前实施状态：

- `5f88743`、`bc69fa1`、`81b157e` 已实现 guest v2 materialization、queue
  endpoint、opaque memref 与 region/view/in-flight lifetime；
- `504f411`、`18f55b2`、`c3bbe93` 已实现 producer/consumer workload、guest image
  wiring 和 `run_ub_dual_node_lingqu_shmem_pto_direct.sh` acceptance CLI；
- `0770b01`、`f3a7e3f`、`182ef9b`、`b2abe0d` 已修正映射同步、OBMM import
  aperture、BQL callback 和实际 callable-1 ABI；
- `4f33311`、`53fd476` 把默认 PTO path 与 NPU/GVA/GSVA 解耦并固定最终 QEMU
  revision；
- n4-910c 与 n4-910c1 r9 均以 `validation.status=pass`、
  `runner_exit_code=0` 结束，退出条件中的 positive synchronous path 全部满足。

退出条件：

- 两节点 producer/consumer 实跑通过；
- Node A 原始 mapping 观察到 PTO `TSTORE` 结果；
- no-staging counters 全为零；
- PTO bytes 与 QEMU UB GM backend bytes 精确相等；
- evidence 记录 QEMU/initramfs/kernel/OBMM/Simpler/PTO hashes；
- 结束后无残留 QEMU。

预计工作量：5–8 个工程日。

### P4：负向、layout 与并发验证（待完成）

| 类别 | Cases |
| --- | --- |
| Mapping | bad handle、stale mapping、wrong requester |
| Lifecycle | retired segment、released import、dispatch 中途取消 |
| Bounds | OOB、整数溢出、shape/stride extent 超界、跨 segment |
| Access | READ memref 上 TSTORE、WRITE memref 上 TLOAD、role 不匹配 |
| Ordering | timeout、remote holder、重复 completion、write fence |
| Layout | tail、cross-page、unaligned、stride、DN、NZ |
| Concurrency | 多 dispatch、多 binding、相同 object 不同 view、读写竞争 |
| Recovery | callback failure、PTO exception、QEMU reset、guest exit |

退出条件：

- 负向 case 返回确定错误；
- 没有 fallback copy 和 partial success；
- layout oracle 与 producer 验证一致；
- 并发 binding 无串扰；
- 远端 guest/QEMU regression 全部通过。

预计工作量：5–8 个工程日。

### P5：性能与上层集成（待完成）

工作内容：

- 分段统计 validate、authorization wait、binding、TLOAD read、layout transform、TSTORE write、fence 和 completion；
- 对比当前 staged `host_vector` 与 `lingqu_shmem` `UB_GM`；
- 评估 tile size、range coalescing 和 callback 数量；
- 接入更高层 Lingqu callable/task submission；
- memref 需要穿越 Simpler L3+ mailbox 时再设计对应 wire extension。

退出条件：

- 正确性 gates 保持通过；
- 每项开销可独立归因；
- QEMU wall-clock 数据不用于推断真实硬件绝对时延；
- 形成可复现的性能报告与启用条件。

预计工作量：4–6 个工程日，不包含完整模型 workload 接入。

## 11. CLI 与验证 gates

### 11.1 统一 CLI

```bash
cargo run --release -p sim-cli -- \
  lingqu-shmem-pto-e2e \
  --scenario 2host \
  --kernel vector-add \
  --elements 1024 \
  --layout nd \
  --verify \
  --evidence-dir out/lingqu-shmem-pto-e2e/run-001
```

CLI 负责：

- 构建或检查 guest app 和 artifact fingerprint；
- 启动两个 QEMU 节点；
- 驱动 export/import/dispatch；
- 汇总两个节点日志和 QEMU counters；
- 执行验收断言；
- 检查残留 QEMU；
- 生成 `validation.json` 和 `validation.status`。

### 11.2 分级 gates

| Gate | 环境 | 必须通过的内容 |
| --- | --- | --- |
| G0 | 本地轻量测试 | ABI、mock callback、bounds、HOST/DEVICE regression |
| G1 | `n4-910c` 单节点 QEMU | existing bridge + local UB GM + PTO `TLOAD/TSTORE` |
| G2 | `n4-910c`/`n4-910c1` 双节点 | OBMM export/import + remote read/write + producer verify |
| G3 | 远端 QEMU | mapping/lifetime/OOB/access/timeout 负向矩阵 |
| G4 | 远端 QEMU | tail/cross-page/stride/DN/NZ |
| G5 | 远端 QEMU | no-staging counters 与 request trace correlation |
| G6 | 空闲远端性能环境 | 分段性能、range coalescing、重复 seeds |

本地开发机只运行已知轻量的 unit/contract/static checks。QEMU、多节点、集成和完整 suit 在远端目标执行。

### 11.3 保留两个 vector regression

两条现有路径都应保留测试，各自承担明确职责：

| Regression | 目的 |
| --- | --- |
| ChipBackend `host_vector` | 防止 QEMU bridge → Simpler/PTO control/execute path 回退 |
| experimental `NPU_OP_VECTOR_ADD_U32` | 显式启用实验 feature 后检查 GSVA device mechanism；不进入默认 gate |
| 新 UB GM vector-add | 证明 existing bridge 与 UB GM access 在同一 `IoOpcode::Dispatch` 中贯通且 no-staging |

## 12. 错误与可观测性

UB GM 分支沿用 `CompletionStatus::FatalFailure { code }`，增加稳定字符串 code：

| Code | 含义 |
| --- | --- |
| `pto_ub_gm_unsupported_callable` | callable 未注册或 artifact fingerprint 不匹配 |
| `pto_ub_gm_bad_control_table` | v2 table version/count/CRC/length 无效 |
| `pto_ub_gm_bad_memref` | dtype/shape/stride/range 无效 |
| `pto_ub_gm_unbound` | PTO 访问未注册 aperture |
| `pto_ub_gm_access_denied` | TLOAD/TSTORE 与权限冲突 |
| `pto_ub_gm_authorization_timeout` | backend mapping authorization 超时 |
| `pto_ub_gm_callback_failed` | QEMU UBC callback 失败 |
| `pto_ub_gm_execution_failed` | Simpler/PTO runtime 失败 |

已有 mapping、lifetime、bounds、access 和 ordering 错误应保留精确信息。所有层共享 `request_id`，并记录 `binding_id`、memref index、callable ID、requester CNA 和 artifact fingerprint。显式启用 experimental adaptor 时，其私有 token/epoch/coherence 错误可以作为附加 diagnostics 返回。

## 13. 主要困难

### 13.1 typed pointer 与 UB GM aperture

PTO CPU `GlobalTensor` 依赖 pointer arithmetic。synthetic aperture 能保留地址计算语义，同时阻止 host dereference。难点在于必须覆盖所有 `TLOAD/TSTORE` layout 分支，任何遗漏都会造成非法宿主访问或静默绕过 capability。

### 13.2 QEMU pending authorization 与同步 bridge

`linqu_uapi_kick()` 的 bridge dispatch 保持同步。P2 已在推进 CMDQ head 前实现 slot
和 control metadata snapshot，并在 timer completion 到达后按 memref cursor 恢复。
binding 只在全部 range 授权成功后注册，进入 PTO callback 后没有 authorization wait。

当前剩余难点集中在 P4 failure semantics：timeout、reset、cancel 和迟到/重复
completion 需要统一销毁 snapshot、撤销已创建资源，并保证 CMDQ/CQ 只完成一次；
多 dispatch 并发还需要证明不同 slot、binding 和 sequence 不串扰。

experimental GSVA adaptor 可以把该内部过程实现成 acquire/ACK；默认状态机和公开接口不使用 GSVA 名称。

### 13.3 callback 的线程与生命周期

callback 从 Simpler/PTO 经 Rust FFI 回到 QEMU。必须固定调用线程、QEMU context 生命周期、reset/cancel 行为和并发策略。V1 建议保持 QEMU UBC BH 上的串行 dispatch，并用 request-scoped registry 限制重入面。

### 13.4 device CNA 的来源

PTO CPU simulator 在宿主执行，语义 requester 仍应代表模拟计算设备。P0 需要在 topology/QEMU device configuration 中提供 `pto_device_cna`。如果现有 attached NPU 与 ChipBackend 表示同一设备，可以显式复用其 CNA；若两者是不同设备，则分配独立 CNA。

验收证据必须记录最终选择，不能默认为 guest CPU CNA。

### 13.5 layout 与 callback 放大

若直接拦截每次 scalar pointer access，FFI 调用量会淹没 workload。V1 应在 tile/range 层聚合 read/write，再复用现有 layout conversion。

### 13.6 多 submodule 协调

改动跨 QEMU、Simpler 和 PTO ISA submodule。每个 submodule 先形成可独立测试、可远端fetch 的 commit，再更新根仓库 gitlink。若改动涉及根级 `mem_service`，其 gitlink 与 `guest-linux/aarch64/mem_service.lock` 必须同步；本 PoC 默认不修改 Memory Service core。

## 14. 工作量与完成度

现有 bridge、ChipBackend/PTO、guest memref 和双节点同步数据路径已经贯通。原始
工作量估算保留如下，实际状态以其后的能力表为准：

| 范围 | 预计工作量 |
| --- | ---: |
| P0：ABI 与契约 | 2–3 天 |
| P1：PTO/Simpler mock UB GM | 5–8 天 |
| P2：现有 QEMU bridge 的 UB GM authorization/callback | 5–8 天 |
| P3：双节点 `lingqu_shmem_memref` E2E | 5–8 天 |
| P4/P5：负向、layout、性能与文档 | 6–10 天 |

当前完成度：

| 能力 | 状态 |
| --- | --- |
| `lingqu_shmem` 上位 GM/TLOAD/TSTORE 需求 | 已定义 |
| `lingqu_shmem`/OBMM 多节点共享内存基座 | 已有实现和既有验证 |
| QEMU UBC → `sim-qemu` → `sim-uapi` | 已实现 |
| `sim-runtime` → `sim-chipbackend-simpler` → Simpler/PTO | 已实现 |
| ChipBackend `host_vector` bridge/PTO E2E | 已实现，数据为 host staging |
| experimental `sim_npu` + GVA/GSVA oracle | 已有实验实现；optional、default disabled |
| `lingqu_shmem_memref` | ABI、runtime view、guest materialization、QEMU parser 和两节点 lifetime 已实现 |
| `AddressSpace::UB_GM` | P0/P1 已实现 C++/Rust/Simpler ABI、128-byte layout tests 与 pass-through |
| Dispatch v2/callback/binding/counter ABI | P0 已实现 header、Rust mirror、malformed metadata validators 与 contract CLI |
| `pto_device_cna` | P0 已加入 scenario config 与 default-disabled property；P2 已按非零 CNA 注册 callback |
| PTO CPU UB GM callback | P1 已实现 contiguous ND、tail、range callback、fail-closed 与 ASan tests |
| existing bridge 的 UB GM authorization/binding | P2 已实现同步 fast path、pending slot snapshot/resume、opaque endpoint-map reference、resume/callback mapping 重校验与 completion cleanup |
| 两节点 PTO direct-access acceptance | n4-910c 与 n4-910c1 r9 均通过；默认 acceptance 无 NPU/GVA/GSVA 依赖 |
| no-staging 结构化证明 | P1 pass-through tests 与 P3 r9 `segment_payload_staging_bytes=0` 共同覆盖；P5 统一 H2D/D2H counters 待完成 |

P0、P1、P2 仿真正向路径和 P3 已完成，最小可信 direct-access PoC 已闭环。P4/P5 决定
负向稳健性、布局覆盖、性能和上层运行时可用性，因此当前仍不能声明第 15 节的
完整目标已经完成。
完整 Lingqu 模型 workload、任意复杂 layout、atomic store 和真实硬件验证不计入该
最小 PoC 估算。

## 15. 最终验收标准

同时满足以下条件后才可以声明目标完成：

- 存在可用的 `lingqu_shmem_memref` API；
- memref materialize 为 `AddressSpace::UB_GM`；
- guest 仍通过 `IoOpcode::Dispatch` 和现有 QEMU/Rust bridge 提交；
- `sim-uapi → sim-runtime → sim-chipbackend-simpler → Simpler/PTO` trace 完整；
- PTO kernel 使用普通 `GlobalTensor<T>` 和真实 `TLOAD/TSTORE`；
- simulator 数据访问经过 PTO device CNA 和通用 QEMU UB GM memory interface；
- mapping、bounds、access、lifetime、ordering 和 fence 生效；
- input/output 均不经过 H2D、D2H 或 host payload staging；
- Node A 从原始 `lingqu_shmem` mapping 验证 Node B 的 PTO output；
- PTO UB GM bytes 与 QEMU UB GM backend bytes 精确匹配；
- 负向 case fail-closed；
- 提供统一 CLI 和自动化测试；
- 双节点 QEMU acceptance 在远端实跑通过；
- evidence 记录完整代码和构建指纹；
- 测试结束后无残留 QEMU。

experimental `NPU_OP_VECTOR_ADD_U32`、GVA 或 GSVA 的启用与通过不计作上述目标完成；默认 acceptance 必须在不依赖这些 experimental features 的条件下成立。

## 16. 待评审决策

实施前需要确认以下代码级决策：

1. `lingqu_shmem_memref` 首先放在 `ub_sim` Lingqu adaptor，接口稳定后再评估进入上游 `libobmm`。本文建议采用该顺序。
2. `IoOpcode::Dispatch` v2 control table 的精确 byte layout、CRC 和最大 memref 数量。
3. `pto_device_cna` 由默认 ChipBackend topology 明确分配，不从 experimental `sim_npu` 隐式继承。
4. synthetic aperture 的编码、每 dispatch binding 上限和并发 dispatch 策略。
5. registry 由 Simpler runtime 管理，PTO CPU ISA 负责 range interception。
6. V1 同时支持 tail tile；DN/NZ 放到 P4。

评审通过后从 P0/P1 开始实施。当前阶段不扩展 attached-NPU opcode，不设计通用 external-memory provider，也不提前修改 Simpler L3+ wire。`sim_npu`、GVA、GSVA 保持 experimental、optional、default disabled，不进入默认 feature set。
