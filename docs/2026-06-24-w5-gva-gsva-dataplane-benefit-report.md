# W5 基于 GVA/GSVA 的数据面收益分析报告

日期：2026-06-24
仓库：`/Volumes/repos/ub_sim`
数据源：`guest-linux/aarch64/out/host_dataplane_microbench.latest.json`
实现入口：`cargo run --release -p sim-cli -- dataplane-microbench`

## 1. 结论

把 legacy baseline 拆成多个实现形态，并用同一组参数完成 host-core 数据面矩阵测量。现在报告不再把 `legacy-pa` 当成单一传统链路代表，而是区分：

1. `legacy-pa-linear`：每次访问线性扫描 PA map entries。
2. `legacy-pa-direct`：单段 direct-style PA->UBA resolve。
3. `legacy-pa-indexed`：按 segment index 做 O(1) range lookup。
4. `legacy-pa-cached`：带单 entry route cache 的 PA->UBA resolve。
5. `generic-gva`：普通 GVA direct window。
6. `gsva`：GSVA identity direct window。

最新稳定样本的核心数字如下：

| 模式 | mixed ns/op | mixed speedup vs linear | mixed 耗时下降 | resolve-only ns/op | resolve speedup vs linear | copy-only ns/op | 混合吞吐 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `legacy-pa-linear` | 16.503 | 1.00x | 0.00% | 13.725 | 1.00x | 1.659 | 3.61 GiB/s |
| `legacy-pa-direct` | 4.819 | 3.42x | 70.80% | 1.874 | 7.32x | 1.665 | 12.37 GiB/s |
| `legacy-pa-indexed` | 5.243 | 3.15x | 68.23% | 2.843 | 4.83x | 1.650 | 11.37 GiB/s |
| `legacy-pa-cached` | 4.522 | 3.65x | 72.60% | 2.130 | 6.44x | 1.644 | 13.18 GiB/s |
| `generic-gva` | 4.277 | 3.86x | 74.09% | 1.928 | 7.12x | 1.656 | 13.94 GiB/s |
| `gsva` | 4.277 | 3.86x | 74.08% | 1.887 | 7.27x | 1.659 | 13.94 GiB/s |

判断：

1. 相对 `legacy-pa-linear`，`generic-gva/gsva` 的 mixed 收益约 `3.86x`。
2. 相对优化过的 `legacy-pa-direct`，`generic-gva/gsva` 的 mixed 收益只有约 `1.13x`。
3. 相对 `legacy-pa-cached`，`generic-gva/gsva` 的 mixed 收益约 `1.06x`。
4. 因此，GVA/GSVA 的大收益主要来自消灭低效 per-access route lookup；如果传统 PA->UBA 路径已经做 direct/index/cache，纯数据面收益会收缩到 `1.0x-1.2x` 区间。
5. `copy-only` 基本不变，说明收益不是 payload copy 或内存带宽带来的，而是 address resolve 路径差异。
6. 这份 benchmark 仍然不能直接推出 W5 端到端 LLM inference 加速比例。端到端收益还取决于 prefix/KV cache 命中率、remote payload 比例、QEMU/guest 调度开销和 LLM compute 占比。

## 2. 要验证的问题

要回答的问题被拆成两层：

1. GVA/GSVA 相对低效软件 route lookup 的收益有多大？
2. GVA/GSVA 相对优化后的传统 PA->UBA resolver 还有多少收益？

第一层对应 `legacy-pa-linear`。第二层对应 `legacy-pa-direct/indexed/cached`。

这不是 QEMU guest 端到端 benchmark，也不是硬件性能测试。它的边界是：

```text
host-core-data-plane qemu=excluded guest_harness=excluded ioctl=excluded scheduler=excluded
```

因此，它只衡量 host Rust 进程里的核心数据面循环：address resolve + payload read/write。

## 3. Benchmark 设计

### 3.1 运行边界

本次 benchmark 排除：

1. QEMU TCG/device model 调度。
2. guest Linux。
3. ioctl/syscall 切换。
4. harness 启动和跨进程同步。
5. LLM compute、tokenizer、sampler。

这样做的目的不是模拟完整系统，而是把数据面核心路径单独拉出来，观察不同地址解析模型的成本。

### 3.2 对比模式

| 模式 | 目标 | 数据面语义 |
| --- | --- | --- |
| `legacy-pa-linear` | pessimistic legacy baseline | local PA -> 线性扫描 map entries -> remote UBA |
| `legacy-pa-direct` | 最强 legacy direct baseline | local PA -> 单段 direct offset -> remote UBA |
| `legacy-pa-indexed` | indexed legacy baseline | local PA -> segment index -> map entry -> remote UBA |
| `legacy-pa-cached` | cached legacy baseline | local PA -> cached segment hit；miss 后 indexed fill |
| `generic-gva` | 普通 GVA direct window | access VA + `pte_offset` -> remote UBA |
| `gsva` | GSVA identity window | access VA == remote UBA，`pte_offset=0` |

### 3.3 参数

| 参数 | 值 | 说明 |
| --- | ---: | --- |
| `size` | 2,097,152 bytes | 远端窗口大小，2 MiB |
| `iterations` | 1,048,576 | 每个 case 的正式循环次数 |
| `chunk_size` | 64 bytes | 每次 read/write payload 大小 |
| `warmup_iterations` | 16,384 | 预热循环 |
| `legacy_map_count` | 64 | legacy map entry 数 |
| `operations` | 2,097,152 | 每个 iteration 包含一次 write resolve 和一次 read resolve |
| `read_bytes` | 67,108,864 bytes | 每个 case 的 read payload 总量 |
| `write_bytes` | 67,108,864 bytes | 每个 case 的 write payload 总量 |

### 3.4 测量项

| 指标 | 含义 | 用途 |
| --- | --- | --- |
| `mixed` | resolve + write payload + resolve + read payload | 最接近真实数据面访问 |
| `resolve_only` | 只执行地址解析，不 copy payload | 单独衡量地址转换成本 |
| `copy_only` | 固定 offset 下只 copy payload | payload copy 对照组 |

`mixed` 判断整体访问成本，`resolve_only` 判断地址解析成本，`copy_only` 用来确认 payload copy 没有成为主要变量。

## 4. 实现原理

### 4.1 统一数据面访问模型

这次 microbenchmark 不是端到端 W5 运行，也不是 QEMU guest 路径。它固定在 host-core 数据面，专门测“地址 resolve + payload read/write”这一层：

```text
logical access address
  -> resolver
  -> remote_uba
  -> remote buffer offset
  -> payload copy
```

所有模式共享同一组 workload 参数：

| 参数 | 值 | 作用 |
| --- | ---: | --- |
| `size` | 2 MiB | 远端对象窗口大小 |
| `chunk_size` | 64 B | 每次读写 payload 大小 |
| `legacy_map_count` | 64 | legacy PA map entry 数 |
| `segment_bytes` | 32 KiB | 每个 legacy segment 大小 |
| `iterations` | 1,048,576 | 正式测量迭代数 |
| `operations` | 2,097,152 | 每次迭代一次 write resolve、一次 read resolve |

`BenchState` 为每个 mode 创建独立状态：

```text
remote:        Vec<u8>, size bytes
write_payload: Vec<u8>, chunk_size bytes
read_payload:  Vec<u8>, chunk_size bytes
resolver:      mode-specific resolver
```

每个 `mixed` iteration 的执行顺序是：

```text
offset = ((iter % chunks) * chunk_size)

write_access = resolve(offset)
copy write_payload -> remote[write_access.remote_offset]

read_access = resolve(offset)
copy remote[read_access.remote_offset] -> read_payload

if verify:
    read_payload must equal write_payload

checksum = fold(remote_uba, token, payload byte)
```

这里的 `checksum` 和 `std::hint::black_box` 用于防止编译器把 resolve 或 copy 优化掉。`verify` 用 payload 等值检查证明不同 resolver 最终访问的是可读写的同一类远端窗口；`verify_failures=0` 是功能正确性的最低证据。

三个计时维度的边界是：

| 维度 | 包含 | 不包含 |
| --- | --- | --- |
| `mixed` | 两次 resolve、一次 write copy、一次 read copy、verify/checksum | QEMU、guest ioctl、scheduler、网络传输 |
| `resolve_only` | 两次 resolve、token/checksum | payload copy |
| `copy_only` | 一次 write copy、一次 read copy、checksum | 地址 resolve |

因此本节所有收益只能解释 host-core 数据面 resolve 成本，不能直接外推为 W5 end-to-end token latency。

### 4.2 `legacy-pa-linear`

`legacy-pa-linear` 使用 `LegacyPaResolver` 的线性扫描：

```text
local_pa -> scan LegacyMapEntry[] -> matching segment -> remote_uba + offset
```

它的数据结构是一个 `LegacyMapEntry[]`，每个 entry 记录：

```text
local_pa_start
remote_uba_start
bytes
token
```

在当前参数下，2 MiB window 被切成 64 个 32 KiB segment。每次访问时，resolver 都从第 0 个 entry 开始扫描：

```text
local_pa = DEFAULT_LOCAL_PA_BASE + offset
len = chunk_size

for index in 0..entries.len():
    entry = entries[index]
    segment_start = entry.local_pa_start
    segment_end = segment_start + entry.bytes

    if local_pa >= segment_start and local_pa + len <= segment_end:
        remote_offset =
            (local_pa - entry.local_pa_start)
            + (entry.remote_uba_start - DEFAULT_REMOTE_UBA_BASE)
        remote_uba =
            entry.remote_uba_start
            + (local_pa - entry.local_pa_start)
        token = validate_legacy_token(entry.token, local_pa, len)
        return ResolvedAccess

miss -> fail
```

这个模式纳入了以下成本：

1. 每次访问的 entry loop。
2. 每个 entry 的 segment boundary check。
3. 命中 entry 后的 remote offset 计算。
4. token validation，用来模拟 metadata 参与访问路径并防止优化。

它不代表“最佳传统实现”，而是代表低效但现实中容易出现的软件模拟路径：数据面每次访问都重新解释 PA route table。如果传统 UB 模拟器在 per DMA、per cache-line、per block copy 上做这类查表，那么 `legacy-pa-linear` 就是合理 baseline；如果真实传统路径已经做了 direct window、index 或 cache，那么这个 baseline 会偏悲观。

### 4.3 `legacy-pa-direct`

`legacy-pa-direct` 代表最强 legacy direct-style resolver：

```text
local_pa -> local_pa - local_pa_base -> remote_uba_base + offset
```

它仍然使用 legacy PA 语义，但假设控制面已经把 PA->UBA map 合并成一个连续 direct window。数据面不再读取 `LegacyMapEntry[]`，只做 window bounds check 和 offset arithmetic：

```text
local_pa = DEFAULT_LOCAL_PA_BASE + offset
total_bytes = segment_bytes * entries.len()

if local_pa < DEFAULT_LOCAL_PA_BASE:
    miss
if local_pa + len > DEFAULT_LOCAL_PA_BASE + total_bytes:
    miss

remote_offset = local_pa - DEFAULT_LOCAL_PA_BASE
remote_uba = DEFAULT_REMOTE_UBA_BASE + remote_offset
token = validate_legacy_token(first_entry.token, local_pa, len)
```

这个模式纳入的成本是：

1. 整个 direct window 的起止边界检查。
2. `local_pa - base` 的 offset 计算。
3. `remote_uba_base + offset` 的 UBA 计算。
4. token validation。

它刻意排除了 per-segment map lookup。它用于回答一个更严格的问题：如果传统 PA->UBA 数据面已经优化成单段 direct aperture，GVA/GSVA 还剩多少收益。

相对 `legacy-pa-direct`，`generic-gva/gsva` mixed 约 `1.13x`。这说明在 direct baseline 下，GVA/GSVA 仍略快，但不再是数量级收益。

### 4.4 `legacy-pa-indexed`

`legacy-pa-indexed` 使用 segment index：

```text
local_pa -> (local_pa - base) / segment_bytes -> entry[index] -> remote_uba
```

它模拟传统 PA->UBA 路径里常见的 page-index 或 range-index 优化：控制面仍然维护多个 map entry，但数据面可以通过 segment index O(1) 定位 entry。

执行路径是：

```text
local_pa = DEFAULT_LOCAL_PA_BASE + offset
total_bytes = segment_bytes * entries.len()

if local_pa outside total window:
    miss

offset = local_pa - DEFAULT_LOCAL_PA_BASE
index = offset / segment_bytes

if local_pa + len crosses entries[index] boundary:
    miss

entry = entries[index]
remote_offset =
    (local_pa - entry.local_pa_start)
    + (entry.remote_uba_start - DEFAULT_REMOTE_UBA_BASE)
remote_uba =
    entry.remote_uba_start
    + (local_pa - entry.local_pa_start)
token = validate_legacy_token(entry.token, local_pa, len)
```

这个模式纳入的成本是：

1. 总 window bounds check。
2. segment index 计算。
3. 单个 entry metadata 读取。
4. segment 内边界检查，确保一次 64B access 不跨 segment。
5. remote offset 和 token 计算。

它比 `legacy-pa-linear` 更公平，因为不再把 route table 长度直接乘进每次访问；但它仍然保留传统 PA->UBA map entry 的数据面 metadata 依赖。这个模式用于判断：如果传统链路已经做了 O(1) route lookup，GVA/GSVA 还能否从“少一次 entry metadata 读取、少一次 segment boundary 逻辑”里拿到收益。

### 4.5 `legacy-pa-cached`

`legacy-pa-cached` 使用单 entry route cache：

```text
local_pa -> cached segment hit -> remote_uba
         -> miss -> indexed fill -> cached segment
```

它模拟传统路径里比较常见的 route cache 或 TLB-like 优化。resolver 保存最近一次命中的 entry index：

```text
cached_entry_index: Option<usize>
```

执行路径是：

```text
if cached_entry_index exists:
    entry = entries[cached_entry_index]
    if local_pa and len are inside entry:
        resolve_entry(entry)
        return

index = index_for_pa(local_pa, len)
cached_entry_index = index
resolve_entry(entries[index])
```

当前 workload 按 chunk 顺序遍历 2 MiB window，每个 segment 是 32 KiB，每个 chunk 是 64B，所以每个 segment 内有 512 个连续 chunk。由于 `mixed` 每个 iteration 对同一个 offset 做 write resolve 和 read resolve，第二次 resolve 通常命中同一个 cached entry；同一 segment 内后续 chunk 也会持续命中，直到跨入下一个 segment。

这个模式纳入的成本是：

1. cached entry 是否存在的分支。
2. cached entry 的 segment boundary check。
3. hit 时的 remote offset 和 token 计算。
4. miss 时的 indexed lookup 和 cache fill。

它是四个 legacy baseline 里最接近“传统路径已经认真优化过”的形态。它不再惩罚/愚蠢的每次访问都扫描 route table，也不会把 segment 数量线性放大到数据面成本里。

本次结果显示：`legacy-pa-cached` mixed 为 `4.522 ns/op`，已经接近 `generic-gva/gsva` 的 `4.277 ns/op`。

### 4.6 `generic-gva`

`generic-gva` 使用固定 `pte_offset`：

```text
access_va + pte_offset -> remote_uba
```

它的数据结构是 `DirectWindowResolver`：

```text
access_base
remote_uba_base
bytes
pte_offset
token
```

其中：

```text
access_base = DEFAULT_GVA_BASE
remote_uba_base = DEFAULT_REMOTE_UBA_BASE
pte_offset = DEFAULT_REMOTE_UBA_BASE - DEFAULT_GVA_BASE
```

执行路径是：

```text
access_va = DEFAULT_GVA_BASE + offset
remote_uba = access_va + pte_offset

if access_va outside [access_base, access_base + bytes):
    miss
if remote_uba outside [remote_uba_base, remote_uba_base + bytes):
    miss

remote_offset = remote_uba - remote_uba_base
token = token ^ rotate(access_va) ^ len
```

它把 mapping lookup 前移到 map 建立阶段。数据面不再解释 PA map，也不读取 route entry；只保留固定 offset 转换、aperture bounds check 和 remote offset 计算。

它和 `legacy-pa-direct` 看起来相近，但语义不同：

| 模式 | 数据面输入 | 控制面假设 | 数据面 metadata |
| --- | --- | --- | --- |
| `legacy-pa-direct` | local PA | legacy PA map 被预合并成 direct window | 仍保留 legacy token 语义 |
| `generic-gva` | guest virtual aperture VA | GVA map 已建立，PTE offset 固定 | 不读取 PA route entry |

所以 `generic-gva` 的价值不是“神奇少做 copy”，而是把地址语义从 PA route table 转成稳定的 virtual aperture offset。

### 4.7 `gsva`

`gsva` 使用 identity address model：

```text
access_va == remote_uba
pte_offset = 0
```

它同样使用 `DirectWindowResolver`，但 aperture base 和 remote UBA base 相同：

```text
access_base = DEFAULT_GSVA_BASE
remote_uba_base = DEFAULT_REMOTE_UBA_BASE
pte_offset = 0
```

执行路径是：

```text
access_va = DEFAULT_GSVA_BASE + offset
remote_uba = access_va

if access_va outside [access_base, access_base + bytes):
    miss
if remote_uba outside [remote_uba_base, remote_uba_base + bytes):
    miss

remote_offset = remote_uba - remote_uba_base
token = token ^ rotate(access_va) ^ len
```

`gsva` 和 `generic-gva` 的 host-core microbench 数字非常接近，这是预期结果：两者在当前模型下都已经是 direct aperture resolve。差异不主要体现在纳秒级 offset arithmetic，而体现在系统语义：

1. `generic-gva` 仍然需要 `access_va + pte_offset`。
2. `gsva` 让访问地址本身就是 remote UBA。
3. prefix cache entry、KV segment ref、跨节点共享对象可以使用同一 global shared virtual aperture。
4. stale 或不匹配的 GSVA segment ref 可以在 metadata 校验阶段 fail-closed，而不是隐式落到另一套 PA route path。

因此 `gsva` 的核心收益不只是本 microbench 里的 `~1.89 ns/op` resolve-only 数字，而是减少 W5 中 prefix/KV/cache/object handoff 的地址语义转换和歧义。

### 4.8 这个实现如何避免“baseline 过低”的误判

如果只拿 `legacy-pa-linear` 对比 GVA/GSVA，会默认传统链路每次访问都线性扫表，这会放大 GVA/GSVA 收益。基于上面章节拆成四类 legacy baseline，结论被约束成两层：

| 问题 | 应看 baseline | 解释 |
| --- | --- | --- |
| 传统链路如果每次访问都解释 route table，GVA/GSVA 能省多少 | `legacy-pa-linear` | 衡量消灭 per-access map scan 的收益 |
| 传统链路如果已经做 direct aperture，GVA/GSVA 还剩多少 | `legacy-pa-direct` | 最强传统 direct baseline |
| 传统链路如果保留多 segment map，但数据面 O(1) 查 entry，GVA/GSVA 还剩多少 | `legacy-pa-indexed` | 衡量少一次 entry metadata 依赖的收益 |
| 传统链路如果有 route cache/TLB-like cache，GVA/GSVA 还剩多少 | `legacy-pa-cached` | 更接近优化传统实现的 baseline |

所以报告里的核心结论不再是单一的 `3.86x`。更准确的表达是：

```text
GVA/GSVA vs low-efficiency linear legacy: 3.86x
GVA/GSVA vs optimized legacy family:     1.06x-1.23x
```

后续如果要和“网络传统多机集群”比较，在 `legacy-pa-linear`之外，还要把 TCP/RPC transport、legacy direct/index/cache、GVA、GSVA 放进同一矩阵。

## 5. 运行命令和日志

正式运行命令：

```bash
cargo run --release -p sim-cli -- dataplane-microbench \
  --iterations 1048576 \
  --warmup 16384 \
  --size 2097152 \
  --chunk-size 64 \
  --legacy-map-count 64 \
  --verify \
  --json guest-linux/aarch64/out/host_dataplane_microbench.latest.json
```

说明：第一次 release build 后的运行曾出现 mixed outlier，但 resolve-only 和 copy-only 正常；后续使用已编译好的 `target/release/sim-cli` 重复运行，矩阵稳定。报告采用最新稳定样本。

稳定样本输出摘要：

```text
dataplane_microbench: status=pass scope="host-core-data-plane qemu=excluded guest_harness=excluded ioctl=excluded scheduler=excluded" size=2097152 iterations=1048576 chunk_size=64 warmup_iterations=16384 legacy_map_count=64
dataplane_case: name=legacy-pa-linear operations=2097152 mixed_ns=34608708 mixed_ns_per_op=16.503 resolve_only_ns=28782625 resolve_ns_per_op=13.725 copy_only_ns=3478625 copy_ns_per_op=1.659 setup_ns=2964959 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0x497e5a181fc19e5
dataplane_case: name=legacy-pa-direct operations=2097152 mixed_ns=10105625 mixed_ns_per_op=4.819 resolve_only_ns=3930750 resolve_ns_per_op=1.874 copy_only_ns=3492750 copy_ns_per_op=1.665 setup_ns=2657041 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0xc1b5e01423b288cb
dataplane_case: name=legacy-pa-indexed operations=2097152 mixed_ns=10994625 mixed_ns_per_op=5.243 resolve_only_ns=5961208 resolve_ns_per_op=2.843 copy_only_ns=3460083 copy_ns_per_op=1.650 setup_ns=2660541 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0x497e5a181fc19e5
dataplane_case: name=legacy-pa-cached operations=2097152 mixed_ns=9482792 mixed_ns_per_op=4.522 resolve_only_ns=4466792 resolve_ns_per_op=2.130 copy_only_ns=3447000 copy_ns_per_op=1.644 setup_ns=2765167 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0x497e5a181fc19e5
dataplane_case: name=generic-gva operations=2097152 mixed_ns=8968625 mixed_ns_per_op=4.277 resolve_only_ns=4043084 resolve_ns_per_op=1.928 copy_only_ns=3473208 copy_ns_per_op=1.656 setup_ns=2657125 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0x5796e1882996179d
dataplane_case: name=gsva operations=2097152 mixed_ns=8968958 mixed_ns_per_op=4.277 resolve_only_ns=3958125 resolve_ns_per_op=1.887 copy_only_ns=3478500 copy_ns_per_op=1.659 setup_ns=2673750 read_bytes=67108864 write_bytes=67108864 verify_failures=0 checksum=0x4010b21c2c6251a4
dataplane_delta: case=legacy-pa-direct baseline=legacy-pa-linear mixed_speedup=3.425
dataplane_delta: case=legacy-pa-indexed baseline=legacy-pa-linear mixed_speedup=3.148
dataplane_delta: case=legacy-pa-cached baseline=legacy-pa-linear mixed_speedup=3.650
dataplane_delta: case=generic-gva baseline=legacy-pa-linear mixed_speedup=3.859
dataplane_delta: case=gsva baseline=legacy-pa-linear mixed_speedup=3.859
```

JSON 中的 speedup 字段名现在是 `speedup_vs_legacy_pa_linear`，避免旧的 `speedup_vs_legacy_pa` 歧义。

## 6. 原始数据

| 模式 | setup ns | mixed ns | resolve-only ns | copy-only ns | read bytes | write bytes | verify failures |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `legacy-pa-linear` | 2,964,959 | 34,608,708 | 28,782,625 | 3,478,625 | 67,108,864 | 67,108,864 | 0 |
| `legacy-pa-direct` | 2,657,041 | 10,105,625 | 3,930,750 | 3,492,750 | 67,108,864 | 67,108,864 | 0 |
| `legacy-pa-indexed` | 2,660,541 | 10,994,625 | 5,961,208 | 3,460,083 | 67,108,864 | 67,108,864 | 0 |
| `legacy-pa-cached` | 2,765,167 | 9,482,792 | 4,466,792 | 3,447,000 | 67,108,864 | 67,108,864 | 0 |
| `generic-gva` | 2,657,125 | 8,968,625 | 4,043,084 | 3,473,208 | 67,108,864 | 67,108,864 | 0 |
| `gsva` | 2,673,750 | 8,968,958 | 3,958,125 | 3,478,500 | 67,108,864 | 67,108,864 | 0 |

Per-operation 数据：

| 模式 | mixed ns/op | resolve-only ns/op | copy-only ns/op | speedup vs linear |
| --- | ---: | ---: | ---: | ---: |
| `legacy-pa-linear` | 16.503 | 13.725 | 1.659 | 1.00x |
| `legacy-pa-direct` | 4.819 | 1.874 | 1.665 | 3.42x |
| `legacy-pa-indexed` | 5.243 | 2.843 | 1.650 | 3.15x |
| `legacy-pa-cached` | 4.522 | 2.130 | 1.644 | 3.65x |
| `generic-gva` | 4.277 | 1.928 | 1.656 | 3.86x |
| `gsva` | 4.277 | 1.887 | 1.659 | 3.86x |

## 7. 收益量化

### 7.1 相对 linear legacy

| 模式 | mixed speedup | resolve speedup | mixed 耗时下降 |
| --- | ---: | ---: | ---: |
| `legacy-pa-direct` | 3.42x | 7.32x | 70.80% |
| `legacy-pa-indexed` | 3.15x | 4.83x | 68.23% |
| `legacy-pa-cached` | 3.65x | 6.44x | 72.60% |
| `generic-gva` | 3.86x | 7.12x | 74.09% |
| `gsva` | 3.86x | 7.27x | 74.08% |

结论：如果传统模拟路径是 per-access linear route lookup，GVA/GSVA 收益非常明确。

### 7.2 相对优化 legacy

| 对比 | mixed ns/op | 加速比 |
| --- | ---: | ---: |
| `generic-gva` vs `legacy-pa-direct` | 4.277 vs 4.819 | 1.13x |
| `gsva` vs `legacy-pa-direct` | 4.277 vs 4.819 | 1.13x |
| `generic-gva` vs `legacy-pa-indexed` | 4.277 vs 5.243 | 1.23x |
| `gsva` vs `legacy-pa-indexed` | 4.277 vs 5.243 | 1.23x |
| `generic-gva` vs `legacy-pa-cached` | 4.277 vs 4.522 | 1.06x |
| `gsva` vs `legacy-pa-cached` | 4.277 vs 4.522 | 1.06x |

结论：如果传统 PA->UBA 路径已经做 direct/index/cache，GVA/GSVA 的纯数据面收益显著收缩，但仍能保持接近或略优。

### 7.3 混合吞吐

每个 case 的 mixed 阶段读 64 MiB、写 64 MiB，总数据量 128 MiB。

| 模式 | mixed 总耗时 | 混合吞吐 |
| --- | ---: | ---: |
| `legacy-pa-linear` | 34.609 ms | 3.61 GiB/s |
| `legacy-pa-direct` | 10.106 ms | 12.37 GiB/s |
| `legacy-pa-indexed` | 10.995 ms | 11.37 GiB/s |
| `legacy-pa-cached` | 9.483 ms | 13.18 GiB/s |
| `generic-gva` | 8.969 ms | 13.94 GiB/s |
| `gsva` | 8.969 ms | 13.94 GiB/s |

### 7.4 收益来源

收益来源按强弱排序：

1. 最大收益：消灭 `legacy-pa-linear` 的 per-access map scan。
2. 中等收益：减少 indexed/cached resolver 中的 metadata lookup 和 cache miss/fill 成本。
3. 小收益：`generic-gva` 到 `gsva` 的 identity offset 简化。本次 mixed 里两者几乎相同，resolve-only 中 `gsva` 略快。

`copy-only` 在所有模式中都位于 `1.64-1.67 ns/op` 左右，说明 payload copy 不是本次收益来源。

## 8. 对 W5 的意义

### 8.1 已经证明的部分

已经证明：

1. GVA/GSVA 相对低效 linear legacy resolver 有 `3.86x` mixed 数据面收益。
2. GVA/GSVA 相对 direct/indexed/cached legacy resolver 的收益区间是 `1.06x-1.23x`。
3. 当前数据面收益主要来自 address resolve，而不是 payload copy。
4. 所有模式 `verify_failures=0`，说明结果不是错误访问或跳过访问造成的假收益。

### 8.2 不能直接推出的部分

不能直接推出：

1. W5 LLM inference 端到端加速是 `3.86x`。
2. prefix cache 命中一定会把整轮 decode 时间降低同等比例。
3. QEMU guest 环境一定能观测到同样幅度。
4. GVA/GSVA 相比所有传统 PA->UBA 实现都有 `3x+` 收益。
5. 网络多机基线一定低于当前 host-core GSVA 数据面。

### 8.3 对 prefix/KV cache 的实际价值

对 W5 prefix/KV cache 来说，GVA/GSVA 的价值仍然成立，但要按两类收益拆开：

| 收益类型 | 来源 | 本报告是否覆盖 |
| --- | --- | --- |
| 执行量减少 | prefix cache / shortpath hit | 不覆盖 |
| 数据面访问变快 | GVA/GSVA direct window / GSVA identity | 覆盖 |

如果 W5 当前瓶颈是 LLM compute 或 QEMU 调度，数据面收益会被稀释。如果 W5 命中 prefix/KV cache 后需要高频消费远端 KV payload，GVA/GSVA 的数据面收益才会在端到端结果中体现。

## 9. 和网络多机基线的关系

网络多机基线通常路径是：

```text
process A -> kernel/network stack -> NIC/TCP or RPC -> remote process -> remote memory
```

当前 host-core benchmark 覆盖的是：

```text
process -> resolver model -> remote simulated memory
```

所以它还不能替代网络多机基线。下一步要比较网络多机，需要同 payload size、同 read/write ratio、同 iteration count、同 warmup、同 correctness verification，把 TCP/RPC、legacy resolver family、GVA、GSVA 放在同一张表里。

## 10. 可信度边界

可信点：

1. 多个 legacy baseline 已经作为正式 CLI modes 实现，而不是手工改参数解释。
2. 三类测量项 `mixed/resolve_only/copy_only` 能拆分收益来源。
3. 所有 case 都带 `--verify`，`verify_failures=0`。
4. JSON 输出字段已明确为 `speedup_vs_legacy_pa_linear`。
5. `legacy-pa` 仍作为兼容 alias 指向 `legacy-pa-linear`，不会破坏旧命令。

边界：

1. 这是 host-core microbenchmark，不是 QEMU guest 端到端测试。
2. 它不包含真实 MMU/TLB/cache coherence 成本。
3. 它不包含真实网络、DMA 或 NIC 行为。
4. 单次 ns 级 benchmark 对 host 调度噪声敏感；正式结论应以重复稳定样本为准。

因此，本报告的正确表述是：

> 在当前 `ub_sim` 的 host-core 数据面模型中，GVA/GSVA 相比 `legacy-pa-linear` 有约 `3.86x` mixed 收益；相比优化后的 `legacy-pa-direct/indexed/cached`，收益收缩到约 `1.06x-1.23x`。这证明 GVA/GSVA 的主要收益来自消灭低效 PA->UBA route lookup，而不是 payload copy。

## 11. 后续计划

后续 W5 性能验证应继续分层：

1. 把 `dataplane-microbench` 的 expanded legacy matrix 固化为回归门禁。
2. 增加多轮 repeat/median/p95 输出，降低 ns 级单样本噪声。
3. 增加 TCP/RPC 网络基线，把网络多机、legacy resolver family、GVA、GSVA 放入同一矩阵。
4. 在 W5 端到端 benchmark 中同时输出 prefix hit rate、KV GSVA bytes、remote payload bytes、decode compute time 和 dataplane time。

这样后续报告可以拆成：

| 层级 | 问题 | 指标 |
| --- | --- | --- |
| host-core dataplane | GVA/GSVA resolve 是否更快 | ns/op、GiB/s |
| legacy resolver family | 相对不同 PA->UBA resolver 有多少收益 | speedup range |
| transport baseline | GVA/GSVA 是否优于网络/legacy UB | latency、throughput |
| W5 e2e | prefix/KV reuse 是否缩短 decode | steps/s、hit rate、total time |
