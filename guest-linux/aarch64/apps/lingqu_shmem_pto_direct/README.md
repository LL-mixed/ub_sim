# Lingqu shmem PTO direct-access workload

This directory defines the guest-side two-node and eight-node acceptance workload for PTO
`AddressSpace::UB_GM` access to `lingqu_shmem`. The source, runner, and runtime
evidence cover the P3 happy path plus the P4 lifecycle, fault, and layout
gates.

- The producer exports one OBMM region, writes two contiguous `f32` inputs,
  initializes the output, publishes bootstrap metadata, and verifies the
  output from the original export mapping.
- The consumer imports that exact region, registers its endpoint map, creates
  three opaque `lingqu_shmem_memref` objects, submits the existing
  `IoOpcode::Dispatch` tag-10 slot, and releases the mapping after completion.

The eight-node functional demo uses Node 0 as producer and Node 1 through Node
7 as consumers. The 2 MiB export contains seven 196,608-byte ND lanes. Each
consumer receives one lane with two 65,536-byte inputs and one 65,536-byte
output, plus a unique requester CNA, operation ID, and request ID. Node 0
verifies all seven output lanes from its original export mapping. The export
also contains seven 64-bit completion acknowledgements after the data lanes.
Each consumer publishes its acknowledgement only after PTO completion has
confirmed the store and fence. Node 0 keeps the export alive until both the
seven output oracles and the seven acknowledgements pass.

Run the eight-node demo through the unified CLI:

```text
sim-cli lingqu-shmem-pto-e2e \
  --manifest /path/to/host_vector_manifest.json \
  --nodes 8 --kernel vector-add --layout nd --elements 16384 --verify \
  --evidence-dir /path/to/new-evidence-directory
```

The underlying command is
`scripts/run_ub_eight_node_lingqu_shmem_pto_direct.sh`. It emits
`validation.json` and `validation.status`, checks 14 loads, 7 stores, 7 fences,
seven successful completions, seven producer-verified lanes, zero payload
staging, stable artifacts, and zero campaign QEMU leftovers. The
acknowledgements carry lifecycle metadata; tensor payload continues to move
only through PTO `TLOAD` and `TSTORE` callbacks.

The current host-vector route holds
`/tmp/linqu_simpler_host_vector.lock`. Seven QEMU consumers may submit at the
same time, while their Simpler callable execution may run sequentially on one
host. The structured report records
`host_dispatch_serialization=possible`; this functional gate does not claim
host callable parallelism.

The artifact fingerprint and PTO requester CNA are required command-line
inputs. The host runner derives them from the selected artifact manifest and
QEMU device configuration; the guest does not guess either identity.

## 受管理对象联合访问模式

新增实现放在本目录的 `lingqu_shmem_pto_managed.c/.h`，沿用本应用与现有
双节点 managed-share 启动基础设施；不增加独立对象服务或模型依赖。
`--managed-config FILE` 选择该模式，`--validate-only` 只检查配置，不打开设备
或建立连接。配置逐行使用 `key=value`，明确指定 `connect`、`key`、`node_id`、
`generation`、`requester_cna`、`artifact_fingerprint` 和 `timeout_ms`；拒绝未知、
重复或缺失字段。第一批固定两节点、单 home、64 KiB 对象及 cross-page 的
`1x64 / 1x64` callable-1 工件，不能把此子集记为完整 AM2 验收。

默认单对象模式中，producer 通过 SDK 分配、acquire 和 managed map，CPU 在原映射初始化 A/B。
consumer 通过同一 key/generation 获取映射，用 provider publish/wait-visible
完成可见性检查，CPU 逐字节核对输入。两次真实 PTO dispatch 分别计算
`C = (A+B+1)*(A+B+2)` 与 `D = (C+B+1)*(C+B+2)`；第二次直接 TLOAD 第一次
TSTORE 的 C，四个视图始终落在同一 backing。CPU 只初始化输入及核对结果，
不向 C/D 写入期望值。控制服务仅收取分配、引用及映射元数据。

成功 completion 后才结束 in-flight 并释放 compute lease；所有 lease 注销后
才 managed unmap、release holder。producer 等待 consumer holder 退出后退役对象。
未知 completion 或清理失败保留所有尚未释放的进程资源并明确报错，测试环境由
上层 runner 终止；不得返回成功或提前把持有者标记为已释放。生成配置、二进制、
完整 guest/QEMU 日志和校验结果放在 campaign 的 `out/`，不进入本源码目录。

构建使用本目录的 `make MANAGED=1`；常规 initramfs 同时启用该模式。
现有 `run_ub_dual_node_apps.sh --app mem_service_object_share --config FILE`
通过配置 `MEM_SERVICE_OBJECT_SHARE_ACCESS=cpu-pto` 选择联合用例，默认 `cpu`
保持原有 SDK 检查。联合模式要求 backend=obmm、size=65536、iterations=1、
fault=none，以及已有 PTO manifest/fingerprint/CNA 配置。它校验 cross-page
工件的真实几何和两个 dispatch 的 callback、backing 身份及 CPU 结果，不复用
旧独立 PTO 用例的通过标记。所有测试失败和未知 completion 都保留原始日志。

可选 `submit_timeout_ms` 启用超时恢复用例，须在 `1..timeout_ms-1` 范围内。
每次 dispatch 必须先超时并保持三条 lease、拒绝 endpoint close，再等待原 op ID
的 completion；不重复提交 slot。缺少预期超时同样判为失败。host 配置通过
`LINGQU_SHMEM_PTO_SUBMIT_TIMEOUT_MS` 传递，默认 0 表示不启用；配合现有
`LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS` 制造迟到完成，authorization timeout
须大于该延迟。校验器的 `--require-timeout-recovery` 要求两次恢复及原有全部数据、
backing、callback 和资源清理证据。

受管理模式现在由 SDK mapping owner 接管同一 holder。`access_threads=0..8`
控制 consumer 的只读 CPU 工作线程数，默认 0；host 配置使用
`LINGQU_SHMEM_PTO_ACCESS_THREADS`。各线程借用同一 backing 的输入 A，在两次
PTO 操作期间持续核对数据。第二次超时拒绝 owner close 和新借用，原 completion
后才释放计算 pin，线程 join 后才关闭映射。普通等待模式单独验证 CPU pin 在用
拒绝关闭。验收器 `--access-threads N` 要求对应数量的线程及 owner 计数证据。
r146/r147 已在双 guest 验证 4 个 CPU 线程与两次顺序 PTO dispatch：延迟和普通
等待均完成真实数据校验、停止接纳后的原映射读取及全部资源清理。r148 通过同一
二进制的旧 direct 入口回归。当前仅覆盖单 home、单对象和 cross-page 256-byte
视图；多请求计算并发、home 互换及 PTO 权限/范围完整矩阵仍待验收，AM2 未完成。

测试专用 `negative_checks=1`（host: `LINGQU_SHMEM_PTO_NEGATIVE_CHECKS=1`）
在两次正常计算前，先验证 bad-mapping-ref、wrong-requester、oob 和
role-access-mismatch 四个预检负例，再运行下面的两个执行负例。预检负例只改变
已生成的测试 metadata，复用旧入口的有界 CRC
重算实现；不更改实际 mapping、backing 或生产 memref API。每次必须获得原
operation 的精确失败 completion，确认所有四个视图保持原值、释放计算 pin，
随后用同一 owner 继续正常计算。该选项暂不与短等待恢复用例组合；默认 0。
校验器必须确认四次预期拒绝没有注册 binding 或执行数据回调，保留全部失败
记录，再核对原有正常计算与资源判据。r150 双 guest 已通过四次精确拒绝、
16 次数据不变检查及同映射的后续两次正常计算；r151 关闭负例开关后通过原
completion 延迟恢复；执行阶段的后续结果见下文，只读页表矩阵仍待验收。

执行阶段扩展的 r154 尝试修改 role/access，真实运行得到 bad_memref，读取字节
为 0，未满足 access-denied 判据。该失败的源码、工件和原始日志保留。

后续扩展沿用专用错误 kernel 的生成路径并保持正常 metadata/signature。工件生成
CLI `--managed-access-fault-request-id N` 将两次错误限定到原 request N 与 N+1：
分别 TSTORE 只读输入和 TLOAD 只写输出。其余 request 执行正常 quadratic kernel；
不使用全局计数器，不改变生产权限检查。manifest 明确记录两个 request，runner
要求它们等于本次 `generation+7/+8`；未开启 negative_checks 时拒绝此类工件。
六个负例之后继续原有两次正常计算，CPU owner 与 backing 始终相同。两个执行
负例均须注册三个 binding、读取 512 bytes、access-denied 完成并注销，写入、
fence 和 staging 均为 0，四视图保持原值。这一扩展不覆盖只读页表。

r155 的第一个执行负例已得到 access-denied、512-byte 读取与零写入；第二个
负例被 Simpler 的重复 callable 注册拒绝。运行时只在整次执行成功后记录已注册
callable，导致前次已确认错误完成后的注册状态丢失。修订将注册成功与执行结果
分别管理，并把参数 signature 纳入缓存键；注册失败仍不占用 ID，完成错误后的
下一次执行复用已注册 callable。

r157 使用重建后的桥接通过完整顺序：六次精确拒绝、24 次数据不变检查、同一
映射的两次正常计算。四个 CPU 线程完成 1174 次校验，停止接纳后各自继续读取，
最终两端资源归零。报告为 8 dispatch、6 rejection（其中 2 次 execution）、
8 LOAD、2 STORE、2 fence，staging 为 0。r158 使用普通工件恢复两次迟到的原
completion；r159 的旧 direct 入口通过。三轮 runner/container 均 exit 0，
独立检查无 QEMU 遗留。当前验收仍限于单 home、单对象和两次顺序正常计算，
`am2_complete=false`。

只读输入扩展（r162c/r163 实际子集已通过）：managed 配置增加
`readonly_inputs=0/1`，host 使用 `LINGQU_SHMEM_PTO_READONLY_INPUTS`，默认关闭。
开启时主 key 保存输入，`<key>-output` 保存结果；两对象各自通过 SDK 分配、
acquire/map 和 owner 管理，consumer 的主对象只请求 READ。CPU 写保护 probe
在映射所属进程执行，工作线程随后启动；CPU/compute 写借用也必须拒绝。
probe 实现位于 `lingqu_shmem_pto_cpu_probe.h`，独立原生 fixture 放在既有 `tests/`。
owner close 的独立 fixture 直接编译应用实现，覆盖两个 owner 的预期拒绝及错误传播。
原有两次 quadratic dispatch 使用输入对象的 A/B 与输出对象的 C/D，第二次
TLOAD 第一次原位 TSTORE 的 C。两个 owner 共用 access domain，各自完成
in-flight/CPU pin、mapping 与 holder 的释放；未知 completion 保留两对象。
原有负例、延迟恢复及 CPU 线程开关继续适用，逐 binding 校验其所属对象与真实
backing。CPU/PTO 对同一输入或输出共用 backing；两个对象具有不同的分配身份。

r162 首轮在完成数据计算后暴露输出 owner guard 返回值错误：预期的新借用拒绝
`-ECANCELED` 被当作整轮错误返回。失败日志保留；修复后原生 fixture 在旧源码
上复现断言失败、新源码上通过。r162c 完成真实 CPU 写保护、六个 PTO 负例、
24 次数据不变检查及两次正常计算，4 线程完成 4922 次校验；r163 在两个 owner
分别保留 1/2 个计算 pin 时恢复原 completion，4 线程完成 4058 次校验。
两轮均验证完整 64 KiB 输入 checksum 不变、逐对象 backing、零 staging 和资源
归零。r164 的默认单对象六负例及 r165 的旧 direct 入口回归通过。
当前仍为单 home、两个 64 KiB 对象、
256-byte 视图和两次顺序正常计算，`am2_complete=false`。

Input publication is ordered by the release fence in `seed_region()` before
the bootstrap record is published. The workload also attempts `msync()` when
the mapping provider supports it. Linux device/PFN mappings can return
`EINVAL` because they have no filesystem writeback operation; the workload
records `msync_unsupported=1` and continues in that case. Any other `msync()`
error remains fatal.

The consumer uses the import aperture address returned by
`obmm_alloc_import_pas()` as the memref's simulator UB GM address. That value
is also passed to the OBMM import operation and QEMU mapping table. Device/PFN
VMAs do not expose a usable PFN through `/proc/self/pagemap`, so the workload
does not attempt to reconstruct the import aperture from its userspace virtual
mapping. Ordinary anonymous metadata pages still use pagemap translation for
the dispatch DMA address.

Example guest invocations:

```text
lingqu_shmem_pto_direct --role producer --node-id 0 --node-count 2 \
  --layout nd --elements 16384 --generation 101 --timeout-ms 120000

lingqu_shmem_pto_direct --role consumer --node-id 1 --node-count 2 \
  --layout nd --elements 16384 --generation 101 --timeout-ms 120000 \
  --requester-cna 0xf002 --artifact-fingerprint 0x1234

lingqu_shmem_pto_direct --role consumer --node-id 1 --node-count 2 \
  --layout nd --elements 16384 --generation 101 --timeout-ms 5000 \
  --cancel-after-ms 10 --expect authorization-cancelled \
  --requester-cna 0xf002 --artifact-fingerprint 0x1234
```

P4H adds three positive layouts while retaining `nd` as the default:

| Layout | Elements | Artifact global/tile geometry | Region placement |
| --- | ---: | --- | --- |
| `nd` | 16,384 | `128x128 / 128x128` | 64-byte aligned contiguous views |
| `tail` | 16,256 | `128x127 / 128x128` | compact row-major global data; final tile column is invalid |
| `cross-page` | 64 | `1x64 / 1x64` | every 256-byte view starts 128 bytes before a 4 KiB boundary |
| `unaligned` | 64 | `1x64 / 1x64` | every view starts at byte offset `4 mod 64` without crossing a page |

The selected layout must match the `ub_gm_layout` object embedded in the
artifact manifest. For example, a tail artifact is built with:

```text
prepare_simpler_host_vector_artifacts.sh /tmp/pto-tail \
  --ub-gm-layout-profile tail \
  --vector-global-rows 128 --vector-global-cols 127 \
  --vector-tile-rows 128 --vector-tile-cols 128
```

The host gate verifies the manifest geometry before boot, exact guest offsets
after boot, and the final QEMU callback addresses and lengths. This prevents a
layout label from passing while the callable executes a different geometry.

The default `--expect success` mode exits successfully only after the producer
observes the transformed output and the consumer receives a successful PTO
completion. `--expect authorization-timeout` is an explicit negative-test
mode. In that mode the producer requires the complete output tensor to retain
the `0x7fc00001` sentinel, while the consumer requires completion status 3 and
the exact `pto_ub_gm_authorization_timeout` code. When those conditions hold,
both roles report the expected failure as a passing test outcome and return
zero, so the PID 1 guest launcher stays healthy. Any changed output, successful
dispatch, or different completion error remains a test failure.

`--expect authorization-cancelled` is the active-cancellation test mode. It
requires `--cancel-after-ms` to fall strictly inside the endpoint timeout.
After the dispatch enters pending authorization, the endpoint writes the
operation identity and then rings the cancel doorbell. It continues polling
until it receives exactly one status-3 completion carrying
`pto_ub_gm_authorization_cancelled`. The producer applies the same complete
sentinel scan used by the timeout test. The host gate additionally proves
that the matching command-queue head advanced once, no authorization or data
callback ran, and an optionally injected late completion was ignored after
the pending snapshot had been destroyed.

The QEMU-only duplicate-completion injection applies to delayed successful
authorization. Each legitimate timer completion resumes the command once;
the immediately repeated completion must be rejected by the active
operation/request/sequence guard with `reason=already_completed`. These
injection switches are host test controls and are absent from the guest
workload ABI.

The host runner's `--reset-on-pending 1` mode starts both guests under QMP,
waits for nodeB to log the first suspended authorization, and issues a strict
`system_reset` to nodeB. QEMU must discard the pending snapshot without a CQ
completion, reject an optional post-reset timer event with `reason=no_pending`,
and retain the monotonic authorization sequence. The rebooted consumer then
submits the workload again and must finish through the ordinary success path.
This makes reset cleanup, stale-event rejection, and post-reset recovery one
end-to-end gate; the guest workload ABI has no reset-specific option.

P4C adds explicit negative-test outcomes `bad-memref` and `access-denied`.
They must be paired with one test-only `--fault-case` value:

- `bad-mapping-ref` changes the generation encoded in the first opaque map
  reference while preserving a valid wire encoding;
- `stale-mapping` retires the registered endpoint map after materialization
  and before submission;
- `released-import` unmaps and unimports the consumer's OBMM import after
  materialization while preserving the registered endpoint-map reference;
- `retired-segment` uses a separate OBMM control export to wait until the
  producer unexports the payload segment, while preserving the consumer
  import, endpoint map, and prepared wire metadata.  The control export is
  one 2 MiB OBMM memseg; only its fixed-size header carries the handshake;
- `wrong-requester` replaces the validated requester CNA with a different,
  syntactically valid CNA;
- `oob` moves the output view one byte beyond the registered map;
- `shape-stride-oob` keeps a valid metadata CRC while extending the output
  shape beyond the declared view;
- `cross-segment` makes one valid view span two adjacent OBMM mappings while
  retaining the source mapping reference;
- `address-overflow` makes the first UB GM range overflow `uint64_t`;
- `role-access-mismatch` declares the output role with read-only access;
- `tstore-on-read` selects a test-only HostVector artifact whose first kernel
  stores its result through the normal read-only `a` input binding;
- `tload-on-write` selects a test-only HostVector artifact whose final kernel
  loads through the normal write-only `f` output binding.

The retired-segment protocol binds a map to the complete source export
lifetime identity: `{owner_cna, remote_uba, token_id, generation,
export_mem_id}`.  The acceptance workload supplies `generation` and
`export_mem_id` through the simulator-private OBMM import ABI v3 and its
independent `SIM_DEC_OP_OBMM_MAP_V2` operation.  The ordinary import ABI v1
remains the default for callers that do not request lifetime tracking.  On
unexport, the kernel calls the simulator provider before releasing the
producer backing; a missing provider or failed tombstone publication makes
unexport fail closed.  Every consumer mapping revalidation checks the exact
five-part shared tombstone, so later reuse of the same UBA or token does not
retire a different export generation.

All metadata mutations happen after the public
`lingqu_shmem_pto_dispatch_prepare()` call and remain inside this acceptance
application. The workload recomputes the ABI CRC over the complete control,
memref, shape, and stride metadata before submission. Consequently, a CRC
failure cannot accidentally satisfy the test. Mapping, requester, bounds, and
role/access mismatch cases must fail during QEMU preflight with no binding or
PTO data callback. Public `lingqu_shmem` callers still cannot construct these
test-only wire objects through the ordinary memref API.

The two execution-stage cases keep the ordinary `[INPUT/READ, INPUT/READ,
OUTPUT/WRITE]` metadata and callable signature. Their manifest carries an
explicit `ub_gm_access_fault` marker, and the artifact fingerprint covers the
faulty kernel binary. The host gate rejects a marker/case mismatch before QEMU
starts. A valid run must pass QEMU preflight, register all three bindings,
enter the callable, and fail only when the test kernel's `TSTORE` or `TLOAD`
requests access excluded by the binding. Both classes require one status-3
completion with the exact expected error, one command-slot retirement, and an
unchanged producer sentinel.

Callable 1 identifies a host-vector artifact whose manifest fixes the selected
global shape and tile capacity. For each logical element it computes `c = a +
b`, followed by `f = (c + 1) * (c + 2)`. The producer verifies this
callable-specific result in its original export mapping. The workload accepts
only the element count assigned to the selected layout, and both host runners
reject a manifest/layout mismatch before QEMU starts.
