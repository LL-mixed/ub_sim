# mem_service Independent Deployment Assessment

## 1. 结论

当前 `mem_service` 已经从 guest-side link-time component 推进为具备独立二进制、Unix-socket daemon、外部 SDK、serving/pretraining 示例、安装布局、打包 smoke、release readiness 与外部认证证据入口的 Memory Service。它已经可以作为本地可发布、可安装、可协同的独立服务形态存在；但还不能宣传为生产环境完全认证的通用 Memory Service，因为真实 Linux systemd/rpm/Prometheus/Alertmanager 和跨主机 remote transport 证据仍然缺失。

更准确的状态是：

| 维度 | 当前状态 | 判断 |
| --- | --- | --- |
| 组件独立性 | 已从 `llm_infer` app 中拆出 `components/mem_service`，并有 model-neutral `mem_service_core.h` | 成立 |
| guest 独立 app | `apps/mem_service` 可构建 core-only `/bin/linqu_mem_service` 和 Qwen3 adapter `/bin/linqu_mem_service_qwen3` | 成立 |
| W5/LLM 协同 | 已支持 Qwen3 range/KV/engram/object handoff 路径 | 成立，但偏 Qwen3/W5 验证路径 |
| 独立发布 | 已有 release/package manifest、wire/admin/API-ABI/compat/upgrade/ops policy contract、version/release-readiness 自描述、tar/deb/rpm package gates、安装后 layout/SDK/runtime smoke、发布包内复验脚本、Linux ops CI wrapper、remote transport CI wrapper 和 release certification verifier；rpm 与真实 ops 认证在无 Linux rpm/systemd 环境时 fail-closed | 本地发布链路成立；生产认证需外部证据 |
| 独立部署 | 已有 `serve --config`、Unix-socket daemon、host daemon、systemd unit 安装布局、runtime config、metrics HTTP listener、collector/alert simulator gate、service-manager lifecycle smoke、storage_root catalog、snapshot+journal、journal fsync/torn-tail recovery/threshold compaction、transactional restore policy、sealed local/chunked/loopback/TCP-loopback block backend | 本地部署链路成立；真实 systemd/collector/alert 需 Linux CI 证据 |
| 任意 LLM serving/pretraining 协同 | 外部进程可通过 installed SDK、pkg-config、typed C client 和 examples 与 daemon 协同；installed SDK runtime smoke 已实际跑通 serving prefix/KV/runtime handoff/execution artifact 与 pretraining artifact/training-step-commit；serving/pretraining fail-closed matrix 和 payload ownership gate 已认证 | 本地协同链路成立；跨主机生产 remote transport 需外部证据 |

所以当前可以把它定位为“本地 release-ready、外部证据 fail-closed 的独立 mem service”。不能把它宣传为“生产环境完全认证”，因为 release-readiness 在缺少 Linux ops bundle 与 remote transport bundle 时仍会输出 `overall_status=not-certified`。

## 2. 已经具备的独立能力

### 2.1 独立组件边界

源码已经位于：

```text
guest-linux/aarch64/components/mem_service/
```

对外入口是：

```text
guest-linux/aarch64/components/mem_service/mem_service.h
guest-linux/aarch64/components/mem_service/mem_service_qwen3.h
guest-linux/aarch64/components/mem_service/lingqu_object_service.h
```

`mem_service.h` 已经暴露基础 metadata/object API：

```text
mem_service_init
mem_service_bootstrap_kvcache
mem_service_update_prefix_metadata
mem_service_get_prefix_group_metadata
mem_service_apply_block_result
mem_service_rebind_block_view
mem_service_handoff_block_owner
mem_service_cluster_fetch_record
mem_service_publish_observe_cluster
mem_service_obmm_service_v0_publish_resolve
mem_service_obmm_service_v0_ensure_cluster_runtime
mem_service_get_record
mem_service_record_to_lingqu_obmm_ref
```

这说明它不是单纯嵌在某个 W5 app 里的静态函数集合，已经有明确的 C component API。

### 2.2 独立 guest CLI

当前有正式 app：

```text
guest-linux/aarch64/apps/mem_service/
```

入口二进制是：

```text
/bin/linqu_mem_service
```

CLI 当前支持：

```text
linqu_mem_service --smoke
linqu_mem_service --self-test
linqu_mem_service wire-fixtures
linqu_mem_service wire-schema
linqu_mem_service wire-schema-fixtures
linqu_mem_service store-fixtures
linqu_mem_service release-manifest
linqu_mem_service release-fixtures
linqu_mem_service serve --listen unix:<path>
linqu_mem_service serve --listen unix:<path> --store <path>
linqu_mem_service health --connect unix:<path>
linqu_mem_service ready --connect unix:<path>
linqu_mem_service status --connect unix:<path>
linqu_mem_service list-records --connect unix:<path>
linqu_mem_service put-object --connect unix:<path> --key <key> ...
linqu_mem_service get-object --connect unix:<path> --key <key>
linqu_mem_service publish-kv --connect unix:<path> --request-id <id> --prefix-group <group> --group-id <id> --block-hash <hash> ...
linqu_mem_service resolve-kv --connect unix:<path> --block-hash <hash>
linqu_mem_service register-prefix --connect unix:<path> --request-id <id> --prefix-group <group> --group-id <id> --block-hash <hash> ...
linqu_mem_service lookup-prefix --connect unix:<path> --request-id <id> --prefix-group <group>
linqu_mem_service publish-runtime-handoff --connect unix:<path> --key <key> ...
linqu_mem_service resolve-runtime-handoff --connect unix:<path> --key <key>
linqu_mem_service register-execution-artifact --connect unix:<path> --key <key> ...
linqu_mem_service query-execution-artifact --connect unix:<path> --key <key>
linqu_mem_service register-training-artifact --connect unix:<path> --key <key> ...
linqu_mem_service query-training-artifact --connect unix:<path> --key <key>
linqu_mem_service_qwen3 --inspect-qwen3
```

`--smoke` 覆盖基础流程：

```text
init
bootstrap kvcache
apply block result
update prefix metadata
get prefix group metadata
get record
validate prefix/group/block relation
```

`--inspect-qwen3` 覆盖 Qwen3 topology inspection：

```text
model_key
pipeline nodes
layer ranges
hidden bytes
decode hidden bytes
kv bytes per token
```

这说明它能作为 guest 中的独立 app 被启动和验证。

当前还具备最小发布布局：

```text
guest-linux/aarch64/apps/mem_service/release-manifest.txt
linqu_mem_service release-manifest
linqu_mem_service release-fixtures
make -C guest-linux/aarch64/apps/mem_service install-smoke DESTDIR=<dir> PREFIX=/usr
```

该布局会安装 core daemon binary、public headers、client SDK source、serving
和 pretraining SDK examples、release manifest、wire schema manifest、config
schema/example 和 deployment manifest，但还
不是完整 package/deploy/upgrade contract。

### 2.3 最小 service process 和 wire contract

当前已新增模型无关服务边界：

```text
guest-linux/aarch64/components/mem_service/mem_service_wire.h
guest-linux/aarch64/components/mem_service/mem_service_daemon.h
guest-linux/aarch64/components/mem_service/mem_service_daemon.c
```

`mem_service_wire.h` 固定了：

```text
magic
version
header_len
request_id
operation
flags
payload_len
payload_checksum
status
error_code
server_time_ms
```

并预留了模型无关 operation：

```text
Health / Ready
Status / ListRecords
PutObject / GetObject
RegisterPrefixEntry / LookupPrefixEntry
PublishKvSegment / ResolveKvSegment
PublishRuntimeHandoff / ResolveRuntimeHandoff
RegisterExecutionArtifact / QueryExecutionArtifact
RegisterTrainingArtifact / QueryTrainingArtifact
```

当前已实现的可运行 operation 是：

```text
Health
Ready
Status / ListRecords
PutObject / GetObject
RegisterPrefixEntry / LookupPrefixEntry
PublishKvSegment / ResolveKvSegment
PublishRuntimeHandoff / ResolveRuntimeHandoff
RegisterExecutionArtifact / QueryExecutionArtifact
RegisterTrainingArtifact / QueryTrainingArtifact
```

runtime handoff、execution artifact、training artifact 现在已经通过同一 envelope 做最小 key/value payload 存取，并在查询侧支持 `expected_version` 和 `expected_checksum`。版本或 checksum 不匹配会 fail closed 为 `STALE_REF` 或 `CHECKSUM_MISMATCH`，不会隐式 fallback 到本地路径。

### 2.4 W5 运行时协同能力

`mem_service_qwen3.h` 暴露的能力已经覆盖 W5/Qwen3 runtime 协同路径：

```text
range input wait/view
scheduler work item wait
range output publish
KV state publish/resolve
terminal token publish/wait
shortpath terminal publish
engram candidate publish/wait
engram selected token wait
engram history/state wait
decode round barrier
```

这些能力已经能服务当前 W5 decode 验证，包括：

```text
prefix metadata
KV state object
hidden range handoff
terminal token object
engram candidates/history/state
OBMM object ref projection
cluster observe
SPSC descriptor exchange
```

所以它已经不是“只能本地存 JSON/metadata”的小工具，而是 W5 runtime object handoff 的关键服务组件。

### 2.5 测试覆盖

当前 Python tests 已覆盖一批结构约束：

```text
guest-linux/aarch64/tests/test_mem_service_record_recycling.py
```

覆盖点包括：

```text
CLI/app layout
record capacity
runtime contract split
compiler annotations split
cluster payload/read/runtime/queue/observe split
object flow split
Qwen3 runtime flow split
record recycling policy
KV payload sizing
object-ref naming
no demo naming regression
```

这对“组件边界不再退回 demo/W5 私有代码”是有价值的。

## 3. 为什么还不能独立发布部署

### 3.1 service process 还只是最小闭环

当前 `/bin/linqu_mem_service` 已有长期运行的 Unix-socket daemon 入口：

```text
linqu_mem_service serve --listen unix:<path>
linqu_mem_service serve --listen unix:<path> --store <path>
```

客户端已有：

```text
linqu_mem_service health --connect unix:<path>
linqu_mem_service ready --connect unix:<path>
linqu_mem_service status --connect unix:<path>
linqu_mem_service list-records --connect unix:<path>
```

并且已经能通过同一 wire envelope 做 object/prefix/KV/runtime handoff/execution artifact/training artifact 最小业务 round-trip。仍缺少：

```text
runtime config reload
real systemd environment deployment smoke
production collector and alert environment integration smoke
product-grade restore admission, rollback, and quarantine policy
graceful shutdown flush policy
multi-client concurrency policy
```

因此它现在能证明“服务进程形态、wire 基础、object/prefix/KV/runtime/execution/training 最小业务 RPC 成立，并且 committed metadata/ref 可通过 `--store` snapshot 恢复、completed idempotency/audit 可通过 `--store` 和 `<store>.journal` 跨重启恢复”，还不能证明“其他 LLM serving 或 pretraining system 已完成产品级协同”。

### 3.2 wire API 还没有发布级 typed payload schema

当前 `mem_service_wire.h` 已经定义了稳定 envelope、operation IDs 和错误模型，`mem_service_wire_client.c/h` 已经把 Unix-socket request helper、default endpoint 和 status name helper 从 daemon 中拆出为轻量 client transport，外部 client 可以不链接 daemon/server/core；`mem_service_client.c/h` 已经在该 transport 上提供最小 typed C wrapper，覆盖 object、prefix、KV、runtime handoff、execution artifact、generic training artifact 的 request/response 结构体，并额外提供 dataset/sample/checkpoint/gradient/optimizer-state/training-step commit pretraining helper。`mem_service_wire_payload.h` 已经把 CLI 和 daemon 的 text key/value payload 读写、整数解析和 schema 校验收敛到共享 helper，`mem_service_wire_schema.h` 已经把当前 operation payload schema 提升成 public contract。`linqu_mem_service wire-schema` 会从 public schema table 生成 `apps/mem_service/wire-schema.txt`，`wire-schema-fixtures` 冻结当前 manifest length/checksum、23 个 operation、102 个字段和 1 个 oneof selector。`mem_service_daemon.c` 已能处理 `Health`/`Ready`、`Status`/`ListRecords`/`Metrics`/`AuditLog`/`InspectObject`/`ExportSnapshot`/`ExportSnapshotPage`/`RestoreSnapshot`/`RestoreSnapshotPage`、object、prefix、KV、runtime handoff、execution artifact、training artifact 最小 RPC；runtime/execution/training artifact query 已支持 expected session/model/kind/id/version/checksum fail-closed 校验；object/prefix/KV/runtime handoff/execution artifact/training artifact 写路径已支持可选 `idempotency_key`，重复相同 operation/payload replay 首次响应，重复 key 搭配不同 payload fail-closed 为 `version_conflict`；completed idempotency record 和 bounded audit record 会随 `--store`、`<store>.journal` 和 full `export-snapshot` 持久化，重启或 full snapshot restore 后仍能 replay/conflict 和查询 audit。`linqu_mem_service wire-fixtures` 现在会校验 header size/offset、operation/status 数值、checksum 算法、header init 行为、23 个当前 RPC 的 canonical request payload 长度/checksum、当前 request schema、23 个真实 handler response 长度/checksum，以及最小 idempotency replay/conflict 行为；`store-fixtures` 覆盖 idempotency save/load 后的 replay/conflict，`journal-fixtures` 覆盖仅从 append-only journal 恢复 completed idempotency 和 retained audit 后的 replay。

缺少：

```text
binary typed payload schema
runtime handoff binary typed request/response schema
execution/pretraining binary typed request/response schema
old-server runtime binary old/new compatibility matrix
product-grade durable idempotency/audit log policy and compatibility matrix
cross-version compatibility tests
old-server runtime binary compatibility matrix
```

当前 `compat-matrix` 冻结本 release 的兼容规则，`compat-baseline-v1` 冻结 old-v1-client 到 current-server 的最小 baseline，`compat-old-new-matrix` 冻结覆盖 23 个 operation 的 v1 old/new schema-profile matrix，`compat-runtime-fixtures` 进一步证明 old v1 minimal client profile 和 current v1 extended client profile 都能访问 current server runtime handler，并覆盖 idempotency replay/conflict 与 fail-closed 计数。它们仍不能替代带旧 server runtime binary 的完整 old/new client/server 组合测试。

这意味着它已经不是纯 in-process API，也不再是散落在 CLI/daemon 里的临时 parser、daemon 私有 schema 或必须链接 daemon 的 client helper；它已经具备最小 object/prefix/KV/runtime/execution/pretraining RPC、request/response payload corpus、lightweight client transport、typed C client wrapper、pretraining SDK helper、共享 payload helper、public operation schema contract 和最小 wire fixture gate。但它还不是完整“独立部署后供其他进程/节点进行 runtime/execution/pretraining 协同”的发布级 typed service API。

### 3.3 持久化边界还不是产品级

当前 guest `mem_service` 的 public state 是进程内 `struct mem_service`：

```text
record_count
records[MEM_SERVICE_MAX_RECORDS]
idempotency_records[MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS]
audit_events[MEM_SERVICE_MAX_AUDIT_EVENTS]
```

它已经能做 runtime metadata/object handoff，并且 `linqu_mem_service store-fixtures`、`linqu_mem_service journal-fixtures` 与 `linqu_mem_service serve --store <path>` 已能把 committed metadata/ref snapshot、completed idempotency record 和 bounded audit record 保存到本地 snapshot 与 `<store>.journal`，并在重启时恢复。`audit-log` 当前可查询 mutating operation 与 fail-closed operation 的 retained ring，并记录 operation/status/request checksum/response checksum/idempotency replay/session/model/artifact/version/checksum。独立 Memory Service 仍需要更强的 durable contract：

```text
durable namespace
durable metadata catalog
payload block store
snapshot/restore
crash recovery
compaction
retention
quarantine
product-grade append-only audit log with truncation/atomicity policy
schema migration
```

Rust 侧已有 `sim-memory` / durable store / prefix cache / execution artifact 相关模型；guest `mem_service` 当前有 snapshot+journal recovery、bounded retained audit ring、`storage_root` durable catalog layout manifest、catalog schema version admission、journal fsync/torn-tail recovery、threshold compaction、transactional restore policy，以及 `sealed-local-block-v1`、`sealed-chunked-block-v1`、`transport-loopback-block-v1`、`transport-tcp-block-v1` 的写入/校验/fail-closed quarantine gate。部署期 `max_records`、`max_payload_bytes`、`retention` 和 `checkpoint_retention` 已有配置解析、非法值 fail-closed fixture 和 release/package manifest contract；`max_records`/`max_payload_bytes` 也已接入 daemon runtime admission，并由 `runtime-quota-fixtures` 与 config runtime daemon 测试覆盖；audit-log retention GC 已接入 daemon runtime admission，`retention=audit-log:<events>` 会裁剪 retained audit ring 并在 GC 后保存 snapshot、强制压缩 journal，`retention-fixtures` 覆盖 durable reload 后旧 audit 不回放；checkpoint record retention 已接入 daemon runtime admission，`checkpoint_retention=latest:<records>` 只裁剪 checkpoint training artifact record，保留非 checkpoint training artifact，并在 GC 后保存 snapshot、清理对应 idempotency record、强制压缩 journal，`checkpoint-retention-fixtures` 覆盖 durable reload 后旧 checkpoint 不回放；checkpoint retention 驱动的 orphan payload block GC 已接入 daemon runtime admission，`payload-gc-fixtures` 覆盖旧 checkpoint payload block 删除、共享 payload block 保留、durable reload 和 journal GC；encryption policy 已显式化为 `encryption=none` / `explicit-none-only`，`encryption-fixtures` 证明 unsupported encryption mode fail-closed，release/package manifest 记录 `encryption_at_rest=not-certified`，避免把未实现加密误报为生产能力；quota/capacity pressure 已通过 `LingquMemServiceCapacityExceeded` Prometheus rule、`alert-fixtures` 与 `alert-integration-fixtures` 接入本地可验证告警契约。剩余缺口不再是本地 durable/remote/chunked backend、基础 runtime quota、本地 audit retention、本地 checkpoint record retention、本地 checkpoint payload orphan GC、本地 encryption admission policy 或本地告警 rule 是否存在，而是真实跨主机 production remote transport evidence，以及更长期的泛化 payload block GC/record retention 策略、实际 at-rest/data-plane encryption/key management 和多版本 catalog migration policy。

### 3.4 模型无关边界还没有完全闭合

当前已经把 Qwen3 相关实现放进 `mem_service_qwen3*.c/h`，并且 core CLI 已经可以不链接 `llm_infer.c`、Qwen3 adapter 源或 guest runtime 聚合源独立构建。`mem_service.h` 也已经改为 `MEM_SERVICE_RECORD_MODEL_*` 通用 record kind，Qwen3 名称只在 `mem_service_qwen3.h` 中作为 adapter alias 存在。

剩余的模型边界问题主要是 adapter CLI 和 runtime flow 仍明显带 Qwen3/W5 验证痕迹：

```text
linqu_mem_service_qwen3 --inspect-qwen3
mem_service_qwen3*.c/h
```

这对当前验证没问题，但对独立服务而言还不够。通用 Memory Service 应该暴露模型无关核心对象：

```text
model_binding
tensor_artifact
kv_segment
prefix_entry
runtime_handoff
execution_artifact
training_sample
optimizer_state_ref
checkpoint_ref
```

Qwen3 应该只是 adapter/plugin，不应该泄漏成核心服务命名。

### 3.5 serving/pretraining 协同 contract 不完整

当前对 inference serving 的支持强于 pretraining。已覆盖较多 decode runtime 对象：

```text
KV state
prefix metadata
range hidden handoff
terminal token
engram state
```

现在已经有最小外部 client 证据：

```text
mem_service_serving_example.c:
  prefix/KV/runtime handoff/execution artifact publish/query

mem_service_pretraining_example.c:
  dataset shard/sample batch/checkpoint/gradient bucket/optimizer state
  training-step commit marker
  typed SDK publish/resolve helper over training artifact RPC

test_pretraining_workers_publish_resolve_and_recover_refs:
  external worker0/worker1 clients publish typed training refs through daemon
  global-step committed marker is published and resolved as training-step-commit
  typed resolve succeeds before and after --store restart
  stale version and checksum mismatch fail closed
  duplicate idempotency key with different payload returns version_conflict

test_cli_training_step_commit_barrier_round_trips_fail_closed:
  commit-training-step/resolve-training-step CLI commands round-trip
  stale expected version fails closed
  duplicate idempotency key with different payload returns version_conflict
```

这些示例和 runtime gate 只链接 `mem_service_client.c` 和
`mem_service_wire_client.c`，测试中作为独立进程连接
`linqu_mem_service serve`，证明服务已经不是只能 in-process 调用的
component。

但 pretraining 和 serving 的产品级协同仍至少需要：

```text
dedicated binary typed schema for dataset/sample/checkpoint/gradient/optimizer
release-grade training step barrier/quorum beyond the current committed marker
replay/audit records
checkpoint lifecycle
multi-writer consistency
model/session/tokenizer mismatch negative matrix
```

这些还没有形成 release-grade public API、wire protocol 或测试矩阵。

### 3.6 发布工程缺口

当前已有最小独立发布工程面：

```text
release manifest
wire schema manifest
compat matrix and baseline artifacts
API/ABI policy artifact
install layout
config file schema
deployment manifest
host daemon artifact under libexec
host systemd-like deployment manifest
host artifact smoke gate
installed host service-manager smoke
installed host collector smoke
SDK examples
metrics export contract
Prometheus alert rules artifact and fixture gate
package manifest and package fixture gate
portable tarball package artifact and package-tarball-smoke gate
arm64 deb package artifact and package-deb-smoke gate
durable catalog layout contract
same-version upgrade/rollback runtime fixture gate
```

但还缺 release-grade 发布门禁：

```text
native rpm package artifact gate (`package-rpm-smoke`, requires Linux rpm toolchain)
Linux ops certification orchestration gate (`linux-ops-certification-smoke`)
old-server runtime binary compatibility matrix
cross-version upgrade/downgrade policy and smoke
real systemd environment smoke
production collector and alert environment integration smoke
security/auth boundary
resource quota
```

所以当前已经能做最小 artifact install/smoke，但仍不能称为可独立发布的产品包。

## 4. 可以如何定位当前 mem_service

当前推荐定位：

```text
mem_service is a guest-side Lingqu memory/object metadata component.
It can run standalone smoke/inspect validation, and it can be linked by
LLM inference guest apps to provide prefix/KV/object handoff for W5.
It is not yet a standalone deployable memory service daemon.
```

中文表述：

```text
mem_service 当前是 guest 内的 Lingqu memory/object metadata 组件，
具备独立 app 验证能力，也能支撑 W5/LLM inference 的 prefix/KV/object
handoff；但还不是可以独立发布部署、被任意 serving/pretraining 系统远程调用的
通用 Memory Service。
```

## 5. 补齐路线

### Phase 1: 固化 service boundary

目标：把“组件 API”固化成“服务 API”。

需要做：

1. 定义 `mem_service_core.h`，只包含模型无关对象和 metadata API。
2. 把 Qwen3 record kind 从 core public enum 中抽出，改成 adapter-owned kind namespace。
3. 定义通用对象类型：
   ```text
   prefix_entry
   kv_segment
   tensor_artifact
   runtime_handoff
   execution_artifact
   memory_record
   training_artifact
   ```
4. 给所有 public structs 加 version/size 字段，形成 ABI 兼容基础。
5. 增加 `linqu_mem_service --self-test`，覆盖 core API，不依赖 Qwen3。

完成标准：

```text
apps/mem_service can build and run core smoke without linking llm_infer.
Qwen3 inspect remains available through adapter mode, not required by core smoke.
```

### Phase 2: 补齐业务 RPC

目标：把当前 `serve`/`health`/`ready` 生命周期闭环补成可被其他进程用于业务协同的服务 API。

需要做：

1. 为当前 `mem_service_wire.h`、`wire-schema.txt`、`compat-matrix.txt`、`compat-baseline-v1.txt` 和 `compat-old-new-matrix.txt` 补齐 old/current client profile 到 current server 的 runtime compatibility fixtures；request/response golden corpus 已由 `wire-fixtures` 覆盖，当前 operation/field manifest 已由 `wire-schema-fixtures` 覆盖，当前 release 兼容规则已由 `compat-fixtures` 覆盖，v1 baseline 已由 `compat-baseline-fixtures` 覆盖，v1 schema-profile old/new matrix 已由 `compat-old-new-fixtures` 覆盖，current server runtime handler 已由 `compat-runtime-fixtures` 覆盖。剩余缺口是带旧 server runtime binary 的完整 old/new client/server 组合测试。
2. 增加 typed request/response payload schema：
   ```text
   PutObject
   GetObject
   PublishKvSegment
   ResolveKvSegment
   RegisterPrefixEntry
   LookupPrefixEntry
   PublishRuntimeHandoff
   ResolveRuntimeHandoff
   RegisterExecutionArtifact
   QueryExecutionArtifact
   ```
3. 实现并测试业务 operation：
   ```text
   PutObject / GetObject
   RegisterPrefixEntry / LookupPrefixEntry
   PublishKvSegment / ResolveKvSegment
   ```
4. 增加 observability/admin endpoint：
   ```text
   /metrics
   linqu_mem_service status          # current minimal read-only admin
   linqu_mem_service list-records    # current minimal read-only admin
   linqu_mem_service metrics         # current minimal read-only admin
   linqu_mem_service metrics-export  # current minimal Prometheus text exporter
   linqu_mem_service inspect-object  # current minimal read-only admin
   linqu_mem_service export-snapshot # current minimal read-only admin, bounded by wire payload size
   linqu_mem_service export-snapshot-page # current minimal read-only admin, paginated by record slot
   linqu_mem_service export-snapshot-to # current minimal page assembly path
   linqu_mem_service restore-snapshot # current transactional paged restore for large snapshots
   ```

完成标准：

```text
LLM serving app can run as a separate process and call mem_service through RPC.
No in-process linking is required for the serving app to publish/resolve KV and prefix entries.
```

### Phase 3: durable backend

目标：让服务重启后不丢关键 metadata 和 payload refs。

需要做：

1. 将当前 `storage_root` catalog layout 扩展为产品级 durable catalog backend。
2. 保持 `sealed-local-block-v1`、`sealed-chunked-block-v1`、`transport-loopback-block-v1` 和 `transport-tcp-block-v1` 的 release gates 继续随 manifest 演进。
3. 维护 transactional snapshot/restore 与 paged restore admission。
4. 维护 append-only audit log、journal fsync/torn-tail recovery 与 threshold compaction。
5. 扩展 schema migration 到多版本 catalog 迁移。
6. 定义 retention/quota/encryption/quarantine/trust policy。

完成标准：

```text
publish prefix/KV/object refs
restart mem_service
resolve the same refs
verify checksum/version/provenance
```

### Phase 4: serving integration

目标：支持独立 LLM serving 系统使用它。

需要做：

1. 定义 serving session namespace。
2. 定义 model binding contract。当前 runtime/execution/training artifact query 已有最小 expected model binding fail-closed 校验，后续还需要扩展到 tokenizer/precision/parallelism/shape 等发布级绑定。
3. 定义 prefix cache API。
4. 定义 KV segment API。
5. 定义 runtime tensor handoff API。
6. 定义 stale/fail-closed behavior。
7. 给 llm serving 做独立 client library。

完成标准：

```text
serving process A publishes prefix/KV/runtime refs
serving process B resolves and consumes them
stale refs fail closed
all refs carry model/session/version/checksum metadata
```

### Phase 5: pretraining integration

目标：支持训练系统，而不只是 decode serving。

需要做：

1. 定义 dataset shard refs。
2. 定义 tokenized sample refs。
3. 定义 activation checkpoint refs。
4. 定义 gradient bucket refs。
5. 定义 optimizer state refs。
6. 定义 training step barrier。
7. 定义 replay/audit records。
8. 定义 checkpoint record retention 和对应 orphan payload block GC；泛化 payload block GC 仍需后续补齐。

完成标准：

```text
pretraining worker can publish and resolve dataset/sample/checkpoint/gradient refs
restart does not lose committed metadata
multi-worker conflict is deterministic
audit log can reconstruct a training step's inputs and outputs
```

## 6. 建议的发布门禁

独立发布前至少需要这些门禁：

| 门禁 | 必须证明 |
| --- | --- |
| core build | `mem_service` core 不链接 `llm_infer` 也能编译和 self-test |
| adapter build | Qwen3 adapter 单独编译，通过 inspect 和 W5 decode tests |
| daemon smoke | `linqu_mem_service serve` 可启动、health ready、graceful shutdown |
| RPC contract | client/server round trip 覆盖 object、prefix、KV、runtime handoff |
| durability | snapshot/restart 后 refs 可解析，checksum/version 不变 |
| stale safety | stale/mismatched refs fail closed |
| serving integration | 独立 serving process 通过 RPC 使用 prefix/KV/object refs |
| pretraining integration | 训练 worker 可发布/解析 sample/checkpoint/gradient refs |
| observability | metrics、audit log、structured errors 可用于定位问题 |

## 7. 当前最应该先做的事

Phase 1 core split 已经完成：`mem_service_core.h` 存在，core CLI 不链接 `llm_infer`，Qwen3 inspect 已进入 adapter-only binary。Phase 2/3 的生命周期、业务最小闭环、最小只读 admin 和最小 restart recovery 也已经开始：`linqu_mem_service serve`、`health`、`ready`、`status`、`list-records`、object、prefix、KV 可以通过 Unix socket 使用同一套 wire envelope，`serve --store` 可以恢复 committed metadata/ref snapshot。

接下来优先级最高的是 Phase 2 和 Phase 3，不是继续堆 Qwen3/W5 特例。

最小可执行切片：

1. 增加 response wire golden fixtures 和 compatibility tests。
2. 把当前 key/value wire payload 从 request golden corpus 推进到 typed encode/decode helpers，并收紧兼容策略。
3. 扩展业务 RPC：
   ```text
PublishRuntimeHandoff
ResolveRuntimeHandoff
RegisterExecutionArtifact
QueryExecutionArtifact
RegisterTrainingArtifact
QueryTrainingArtifact
   ```
4. 写 client CLI 和 Python contract tests。

完成这一步后，才能说：

```text
mem_service can be deployed as a standalone service for LLM serving integration.
```

pretraining 协同还需要 Phase 5 的训练对象和一致性语义补齐。
