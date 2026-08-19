# OBMM submodule 引入与迁移设计

- 日期：2026-08-19
- 状态：已确认（设计方案已逐节评审通过）
- 范围：引入 openeuler/obmm 为 submodule；guest-linux 与 mem_service 的基础
  OBMM export/import 封装改为基于 libobmm；sim 专属扩展保留本地薄层

## 1. 背景与目标

ub_sim 与 mem_service 中存在多份手写的 OBMM ioctl 封装
（`guest-linux/aarch64/common/obmm_common.h` 及其在 mem_service 中的
vendored 拷贝）。openeuler 上游仓库 obmm 提供了正式的用户态库
libobmm，覆盖 export/import/unexport/unimport 基础流程。目标：

1. 以 submodule 形式引入上游 obmm，作为基础流程的统一底座；
2. 本地与 vendored 的私有封装改为基于 libobmm（适配层方式，调用方零改动）；
3. sim 专属能力（GSVA、bootstrap、SPSC 队列、窗口 PA 分配等）保留在本地。

## 2. 关键技术事实（调研结论）

- 上游 libobmm 与 ub_sim kernel_ub 驱动使用同一套基础 ioctl UAPI
  （`OBMM_CMD_EXPORT/IMPORT/UNEXPORT/UNIMPORT`），基础结构体布局一致；
  kernel_ub 的 UAPI 头是超集（新增 GSVA flag、bootstrap ioctl 等）。
- `libobmm.h` include `<ub/obmm.h>`；atomgit origin/master（53011ee）
  未带 vendored UAPI 头（那是本地 fork 领先 commit 9f5a8e7 加的），
  因此编译时需以 `-I kernel_ub/include/uapi` 提供超集头。
- `obmm_export()` 回填 `desc->addr`(=uba) 与 `desc->tokenid`，
  与本地 `obmm_do_export` 填 `obmm_helpers_meta` 的字段一一对应。
- `fill_import_cmd_info` 按 `desc->priv/priv_len` 透传 priv blob；
  sim 的 priv v1/v2（MESI/GSVA 元数据）可原样通过，内核侧
  `obmm_sim_dec_parse_import_priv` 解析逻辑不需要动。
- `obmm_export()` 无 requested_uba 输入路径，fixed-uba export 无法经
  libobmm 表达，保留 raw ioctl。
- 上游 `vendor_adaptor.c` 为华为硬件专用（拒绝全零 EID、glob
  `/sys/devices/ub_bus_controller*`），在 sim guest 中必然失败。
  `vendor_adaptor.h` 是按厂商抽象设计的链接接缝，提供 sim 版实现即可
  替换，submodule 无需修改。
- 上游无 SPSC 队列、bootstrap、GSVA、窗口/网络助手等对应物。
- 本地 fork（gitcode/github）领先 atomgit 4 个 commit（加固、测试框架、
  vendored UAPI 头、logging 规范化），本次决定 pin atomgit tip。

## 3. 已确认决策

| 决策点 | 结论 |
|---|---|
| 迁移深度 | 适配层替换：`obmm_common.h` 函数签名不变，内部改调 libobmm；14 个 guest app 与 mem_service provider/cluster_runtime 调用方零改动 |
| submodule 版本 | URL `https://atomgit.com/openeuler/obmm.git`，pin origin/master 53011ee，branch master；libobmm 编译用 kernel_ub 扩展 UAPI 头 |
| vendor adaptor | 新增 sim 版 `obmm_vendor_adaptor_sim.c`，链接时替换硬件版；submodule 一字不改 |
| mem_service 依赖 | mem_service 自身引入同一 submodule（同 URL 同 pin），保持独立构建；不引用 ub_sim 的 vendor 目录 |
| obmm_queue | 原样保留（上游无对应物） |
| 推送顺序 | mem_service 本地 commit+测试 → 用户确认 push → 之后才 bump ub_sim gitlink 与 mem_service.lock |

## 4. 总体架构

```
vendor/obmm (submodule, pin 53011ee, 不可修改)
├── src/libobmm/libobmm.c        ← 直接编译进目标
└── src/libobmm/vendor_adaptor.h ← 仅使用头文件

guest-linux/aarch64/common/
└── obmm_vendor_adaptor_sim.c    ← sim 厂商实现（新增）
    vendor_adapt_export()        → vendor_info=NULL, pxm_numa=0
    vendor_fixup_import_cmd()    → no-op
    vendor_fixup_preimport_cmd() → no-op
```

分层：

| 层 | 内容 | 来源 |
|---|---|---|
| L2 sim 扩展 | bootstrap、GSVA mmap、窗口 PA 分配、ipourma 网络助手 | obmm_common.h 原样保留 |
| L1 适配层 | `obmm_do_*` 家族，签名不变，内部组装 `obmm_mem_desc` 调 libobmm | obmm_common.h 改写 |
| L0 底座 | `obmm_export/import/unexport/unimport` | vendor/obmm submodule |

## 5. guest-linux 侧改造

`obmm_common.h` 内部分流（全部签名不变）：

| 函数 | 去向 |
|---|---|
| `obmm_do_export` / `obmm_do_unexport` / `obmm_do_import` / `obmm_do_import_v2(_epoch)` / `obmm_do_unimport` | 组装 `obmm_mem_desc`（addr/length/tokenid/scna/dcna + priv blob 走 `desc->priv`）调 libobmm |
| `obmm_do_export_fixed_uba` | 保留 raw ioctl（libobmm 无 requested_uba 输入路径；GSVA sim 扩展） |
| bootstrap publish/lookup、GSVA mmap、窗口 PA 分配、网络助手、`obmm_open_device` | 不动 |

构建接入：新增 `guest-linux/aarch64/common/libobmm.mk`（提供
`OBMM_CFLAGS`/`OBMM_SRCS` 变量：libobmm.c + sim adaptor 两个源文件、
libobmm 与 kernel_ub UAPI 两个 `-I`）；每个使用 obmm_common.h 的 app
Makefile include 之并把 `$(OBMM_SRCS)` 加入链接。`init.c`、
`libs/obmm_queue`、kernel 驱动、run 脚本不动。

## 6. mem_service 侧改造

```
mem_service/
├── vendor/obmm          ← 新增 submodule（同 URL，pin 53011ee）
├── common/obmm_common.h ← 与 ub_sim 同步改写（保持独立构建的 include 调整）
├── common/obmm_vendor_adaptor_sim.c ← 新增
├── libs/obmm_queue/     ← 保留
├── kernel_ub/.../obmm.h, gsva.h ← 保留 vendored（bootstrap ioctl + 编译超集头）
└── VENDORED.md          ← 刷新 ub_sim 源 revision + SHA-256
```

- provider 与 cluster_runtime 零代码改动；需要改的是编译入口
  （pytest `_compile()`、scripts 构建命令）：加 libobmm 两个 `-I`
  与两个源文件。
- 遵循 VENDORED.md 流程：先改 ub_sim → 同步拷贝（含 include 调整）→
  刷新 revision/checksum → standalone 套件 + 下游 contract 测试。
- ub_sim 侧：AGENTS.md 规定 gitlink 与 `guest-linux/aarch64/mem_service.lock`
  必须一起更新，且仅在 mem_service 有 clean、tested、remotely
  fetchable 的 commit 之后；push 需用户确认。
- `git submodule update --init` 升级为 `--recursive`（嵌套 submodule），
  同步更新 AGENTS.md。

## 7. 测试与验证

按 AGENTS.md：本地默认轻量检查，QEMU 级验证限远程主机。

1. 适配层字节等价单测（新增，host）：以 `-Wl,--wrap=ioctl` 拦截
   ioctl，断言新适配层发出的 `obmm_cmd_export/import` 逐字段等于旧
   实现的 golden 期望（uba/tokenid/scna/dcna/priv v1/v2 布局）。
   落在 `guest-linux/aarch64/tests/`。
2. 构建矩阵：`run_ub_app_build_matrix.sh`（aarch64 工具链可用时）；
   否则降级为 host `cc` 编译所有含 obmm_common.h 的 TU。mem_service
   standalone：`python3 -m unittest discover tests`。
3. 契约测试（新增+扩展）：submodule 声明与 pin 断言；Makefile 必须含
   libobmm.mk 并链接 sim adaptor；mem_service VENDORED.md 校验和一致
   （扩展既有 `verify_mem_service_source.py` / `test_repository_contract.py`
   机制）。已有 ub_sim guest 测试全绿。
4. Guest 真实 datapath：由既有远程验证流程覆盖（`run_ub_dual_node_*`），
   交付时给出建议脚本清单，由用户决定触发时机。

完成标准：第 1–3 层本地全绿；Rust workspace 不受影响。

## 8. 实施顺序

1. ub_sim：添加 `vendor/obmm` submodule（pin 53011ee）
2. ub_sim：sim adaptor + `libobmm.mk` + `obmm_common.h` 适配层改写 + app Makefile 接线
3. ub_sim：字节等价单测 + 契约测试 + 构建矩阵，全绿
4. mem_service：同名 submodule + 同步 obmm_common.h + adaptor + 编译入口
5. mem_service：standalone 测试全绿 + VENDORED.md 刷新，commit
6. 停下：请用户确认 push mem_service
7. push 后：bump ub_sim gitlink + mem_service.lock，收尾提交
