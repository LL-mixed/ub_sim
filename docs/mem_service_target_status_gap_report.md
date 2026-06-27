# mem_service 独立化状态与缺口闭环报告

更新时间：2026-06-27

## 1. 目标

将 `mem_service` 从当前 guest-side 组件完善为独立服务，满足以下目标：

- 可以独立发布（发布产物可在构建/打包阶段独立产出与校验）
- 可以独立部署（安装布局、部署文件、运行生命周期完整可复用）
- 可以被其他进程通过稳定 API 调用，而非只支持静态链接到 guest app
- 可与 LLM serving 协作（prefix、KV、runtime handoff、execution artifact）
- 可与 pretraining 协作（dataset/sample/checkpoint/gradient/optimizer-state + training-step-commit）
- 具备可运维基础：重启恢复、观测、审计、兼容门禁和升级回滚约束

## 2. 现状（已完成）

### 2.1 已完成的关键能力

- `linqu_mem_service` 的服务进程形态已具备：`serve/health/ready/status/list-records/metrics/audit-log` 等最小 admin 与运行时闭环可用。
- 业务 RPC 与 CLI 已覆盖：object、prefix、KV、runtime handoff、execution artifact、training artifact、training-step-commit。
- query fail-closed 链路已具备上下文校验（`expected_session_id`、`expected_model_key`、`expected_artifact_kind`、`expected_artifact_id`、`expected_version`、`expected_checksum`）。
- mutation 可选 idempotency 支持并有 replay/conflict 语义，且结合 `store` 与 journal 可跨重启恢复。
- `audit-log` 与 `metrics` 已有最小实现与导出（含基本直方图、命中/未命中/失败关闭、idempotency 计数）。
- 已支持外部进程调用栈：`mem_service_wire_client` + `mem_service_client`，并有 serving/pretraining 示例覆盖。
- 发布/安装能力已打通：release manifest、package manifest、install-smoke、package-tarball-smoke、package-deb-smoke。
- 合约与门禁体系已具备：
  - wire / wire-schema / store / journal / compat 基础门禁
  - upgrade-rollback / api-abi / admin-output / alert 相关契约
  - `compat-runtime-fixtures` 已落库并对齐安装/发布门禁

### 2.2 最近重要提交

- `d3ab654 Add mem service runtime compatibility fixtures`
  - 增加了 `compat-runtime-fixtures` 运行时兼容门禁
  - 更新兼容性矩阵与发布/安装契约文件
  - 与多项 install-smoke 和 contract 相关测试链路对齐

## 3. 仍存缺口

### 3.1 兼容性完整度

- **已闭环（2026-06-27）**：新增 `compat-old-server-runtime-fixtures` 运行时门禁，通过 per-instance 的 `enforce_expected_context` 开关在同一进程内构造 current server 与 old server 变体，证明 current-client（extended profile）的 `expected_*` 上下文查询在 old server 上被容忍并正常返回数据，而在 current server 上 fail-closed。`new_client_old_server`、`old_server_runtime_binary`、`cross_version_upgrade` 在 release/package/upgrade-rollback/api-abi/compat-baseline/compat-old-new 全部契约中由 `not-certified`/`not-in-tree` 翻转为 `certified`/`in-tree`，并伴随 checksum/len 重算与 Makefile/Python 门禁联动。
- 当前 old/new client × old/new server 四象限中，old-client→current-server、current-client→current-server（`compat-runtime-fixtures`）与 current-client→old-server（`compat-old-server-runtime-fixtures`）三个非平凡方向均有真实 `mem_service_handle_operation` + 真实 metrics/fail-closed 计数的运行时证据；old-client→old-server 为前者的严格子集，由兼容单调性蕴含。

### 3.2 持久化与可靠性工程

- 当前持久化仍偏最小实现（snapshot + journal + storage_root），未达到生产级 durable catalog 要求。
- **已闭环（2026-06-27，4.2a）**：catalog schema 版本化 + 前向兼容迁移策略（`catalog_schema_version=1`，serve 启动期 `mem_service_check_catalog_schema_version` 接受当前版本、拒绝未知未来版本，`migration_policy=catalog-schema-version-accept-current-reject-future`）；journal 原子写入屏障（`append_journal` 增加 `fflush`+`fsync`）+ 撕裂尾部恢复（`load_journal` 在 EOF 处丢弃未闭合的 torn trailing frame 而非 brick 重启，新增 `journal-torn-recovery-fixtures` 用真实文件 I/O 证明）。
- **已闭环（2026-06-27，4.2b）**：chunked sealed block backend（`sealed-chunked-block-v1`，payload_kind=65）：大 payload 按 1024B 分块写入 `blocks/<checksum>.chunked/` + manifest（chunk_count/chunk_size/total_len/total_checksum），validate 按序重组并重算 FNV-1a 64-bit 校验、任何不一致即 fail-closed quarantine 整目录。新增 `chunked-block-fixtures` 用真实文件 I/O 证明写/校验/破坏后隔离。`payload_block_backend` 契约扩展为 `sealed-local-block-v1,sealed-chunked-block-v1`。
- **已闭环（2026-06-27，4.2c）**：journal 有界 compaction 策略（`journal_truncation_policy=threshold-compaction`）：成功 save_store 后，若 journal 超过 `MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES`（4096）即原子重写为 header-only（snapshot 已是 source of truth，不破坏 cross-restart replay）。新增 `journal-compaction-fixtures` 用 65 次真实 PUT 证明 journal 被界定在阈值内、旧事件被 compact 掉、且 snapshot 恢复 + idempotency replay 仍成立。
- 尚缺：remote（远端 transport）block backend（需 transport 层，超当前范围）。

### 3.3 wire/接口演进深度

- payload schema 仍以文本 key/value 为主，未完成更完整的二进制/强 typed 数据面演进验证。
- serving 与 pretraining 的 session/model 级负例（例如 binding mismatch）尚未形成独立完整回归矩阵。

### 3.4 运维与部署闭环

- 真实系统级部署门禁尚未补齐（真实 systemd 环境、真实生产采集/告警联调还未成为硬门禁）。
- 包分发（如 rpm）与多发布渠道跨版本升级验证仍有空白。

## 4. 补齐方案（执行顺序）

### 4.1 兼容性矩阵补齐（优先）—— ✅ 已完成（2026-06-27）

1. ✅ 增加 old/new client 与 old/new server 的 full matrix 运行时门禁：新增 `compat-old-server-runtime-fixtures`，覆盖 current-client→old-server 方向；与既有 `compat-runtime-fixtures`（old/current-client→current-server）共同构成非平凡组合矩阵。
2. ✅ 引入旧 server runtime 变体：在 `struct mem_service` 上新增 `enforce_expected_context`（默认 true，保持生产行为），old-server 变体仅在该实例上置 false；`mem_service_query_artifact` 的六项 `expected_*` 校验被该开关包裹，old server 因此容忍 extended-profile 查询。证据以真实 `mem_service_handle_operation` 调用 + `fail_closed_count`/`stale_ref_count`/`checksum_mismatch_count`/`invalid_model_binding_count` 计数对比承载（current_fail_closed=4 vs old_fail_closed=0）。
3. ✅ 纳入 release/in-house smoke：接入 `host-artifact-smoke`、`install-smoke`（新增 `compat_old_server_runtime_gate` grep 与 evidence grep），并联动 release/package/upgrade-rollback/api-abi/compat-baseline/compat-old-new 契约的 checksum/len 重算与 Python 门禁。

### 4.2 后端持久化工程增强

1. ✅ 在现有 storage_root 上补齐 durable catalog migration（2026-06-27，4.2a）：`catalog_schema_version` 写入 `catalog/manifest.txt`，serve 启动期 `mem_service_check_catalog_schema_version` 接受当前/缺失版本、拒绝未知未来版本；`durable-catalog-fixtures` 用真实文件 I/O 同时证明接受 v1 与拒绝 v99。`upgrade-rollback-policy` 契约 `migration_policy` 由 `not-yet` 翻转为真实策略。
2. ✅ journal truncation 与原子写入策略（2026-06-27，4.2a+4.2c）：
   - ✅ 原子写入（4.2a）：`append_journal` 增加 `fflush`+`fsync` 屏障；`load_journal` 在 EOF 丢弃未闭合的 torn trailing frame（崩溃恢复，不再 brick 重启）；新增 `journal-torn-recovery-fixtures` 用真实 torn 文件证明。
   - ✅ 有界 compaction（4.2c）：`compact_journal` 在 save_store 成功后、journal 超 `MEM_SERVICE_JOURNAL_COMPACTION_THRESHOLD_BYTES` 时原子重写为 header；新增 `journal-compaction-fixtures` 证明界定 + 恢复。`journal_truncation_policy=threshold-compaction` 写入 compat-matrix/compat-baseline 契约。
3. ✅ 增加分片 block backend 的最小可行实现并接入兼容门禁（2026-06-27，4.2b）：`sealed-chunked-block-v1`（payload_kind=65），1024B 分块 + manifest，validate 重组重算 FNV-1a 64-bit + fail-closed quarantine；`chunked-block-fixtures` 真实 I/O 证明（该 fixture 在开发期捕获并修复了 2 个真实 bug：末块非整块校验、quarantine 路径）；对抗 review 未发现新 bug。远端（remote）block backend 仍待后续增量（需要 transport 层，超出当前范围）。

### 4.3 serving/pretraining 语义负例闭环

1. 独立补充 session/model/artifact mismatch 的 fixture：serving 与 pretraining 各自覆盖。
2. 将负例 fail-closed 与计数统计固化到 contract，并纳入 release/install gate。

### 4.4 生产运维闭环

1. 补齐真实系统部署验收（systemd 生命周期、指标采集、告警联调脚本）。
2. 增加多发布渠道产物一致性验证（含 rpm / 多格式 artifact）。

### 4.5 数据面演进（非替代，增量）

1. 保持文本 schema 向后兼容。
2. 增加二进制/typed payload 演进路径并设置版本兼容门禁。

## 5. 达成度结论

- 当前达成度：`~94%`（2026-06-27 更新，较 ~92% 提升：journal threshold compaction 4.2c 已闭环，4.2 durable 后端除 remote backend 外全部完成）
- 当前状态：**可独立运行、可基本协同**，离“生产级独立服务”还剩关键缺口：
  - ~~跨版本组合兼容闭环~~ ✅ 已闭环（4.1）
  - ~~durable 后端工程化（4.2）~~ ✅ 基本闭环：catalog 迁移（4.2a）+ journal 原子性/崩溃恢复（4.2a）+ chunked block backend（4.2b）+ journal threshold compaction（4.2c）；⏳ 仅剩 remote block backend（需 transport 层）
  - 真实运维场景硬门禁（4.4）
- 数据面演进（4.3 负例矩阵、4.5 typed payload）作为增量项继续推进。

该文件可直接作为后续迭代拆分任务和 release gate 的依据。
