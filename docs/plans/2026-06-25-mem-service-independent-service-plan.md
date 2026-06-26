# mem_service Independent Service Implementation and Evaluation Plan

## 1. 目标

把 `mem_service` 从当前 guest-side link-time component 补齐为可以独立发布、独立部署，并能与其他 LLM serving 和 pretraining 系统协同工作的 Lingqu Memory Service。

目标完成时，`mem_service` 必须同时满足：

1. 可以作为独立 artifact 发布。
2. 可以作为长期运行的 service process 部署。
3. 可以被其他进程通过稳定 API 调用，而不是只能被静态链接进 guest app。
4. 可以服务 LLM inference serving 的 prefix cache、KV cache、runtime object handoff、execution artifact lookup。
5. 可以服务 pretraining 的 dataset/sample/checkpoint/gradient/optimizer-state metadata 协同和 global-step committed-state 协同。
6. 具备 durable metadata、payload ref、audit、restart/recovery、version compatibility 和 observability。
7. Qwen3/W5 只是 adapter 和验证 workload，不再定义服务边界。

## 2. 当前状态

当前代码已经具备以下基础：

| 能力 | 当前证据 | 状态 |
| --- | --- | --- |
| 组件目录 | `guest-linux/aarch64/components/mem_service/` | 已有 |
| public C API | `mem_service.h`, `mem_service_core.h`, `mem_service_qwen3.h`, `lingqu_object_service.h` | 已有 |
| standalone guest app | `guest-linux/aarch64/apps/mem_service/` | 已有 |
| smoke CLI | `/bin/linqu_mem_service --smoke` | 已有 |
| self-test CLI | `/bin/linqu_mem_service --self-test` | 已有 |
| core-only app build | `linqu_mem_service`, `linqu_mem_service_core` 不链接 `llm_infer.c`、Qwen3 adapter 源或 guest runtime 聚合源 | 已有 |
| model-neutral core record kinds | `mem_service.h` 使用 `MEM_SERVICE_RECORD_MODEL_*`，Qwen3 名称只在 adapter header 中做 alias | 已有 |
| wire envelope | `mem_service_wire.h` 定义 versioned header、operation IDs、payload checksum、stable status/error code | 已有最小版本 |
| lightweight RPC client transport | `mem_service_wire_client.c/h` 提供 Unix-socket request helper、default endpoint、stable status name、显式 timeout、opt-in max-attempts/backoff/timeout-retry 控制；外部 client 可不链接 daemon/server/core | 已有最小版本 |
| typed C client API | `mem_service_client.c/h` 提供 object、prefix、KV、runtime handoff、execution artifact、generic training artifact 的 typed request/response wrapper，并提供 dataset/sample/checkpoint/gradient/optimizer-state 和 training-step commit pretraining helper；同时透传 transport timeout/retry options；测试证明外部 client 只链接 client + wire client 即可连通 daemon | 已有最小版本 |
| serving SDK example | `examples/mem_service_serving_example.c` 只链接 client + wire client，显式配置 timeout/retry/backoff，并通过 idempotency key 安全发布 prefix/KV/runtime handoff/execution artifact | 已有最小两进程 smoke |
| pretraining SDK/example/runtime gate | `examples/mem_service_pretraining_example.c` 只链接 client + wire client，显式配置 timeout/retry/backoff，并通过 pretraining typed helper 和 idempotency key 发布 dataset/sample/checkpoint/gradient/optimizer-state training refs 以及 `training-step-commit` committed marker；`test_pretraining_workers_publish_resolve_and_recover_refs` 编译外部 worker client，覆盖 worker0/worker1 多进程 publish、typed resolve、global-step commit marker、checkpoint restart recovery、stale/checksum fail-closed 和 idempotency conflict；`test_cli_training_step_commit_barrier_round_trips_fail_closed` 覆盖 CLI commit/resolve/stale/conflict | 已有最小多进程证据 |
| wire fixture gate | `linqu_mem_service wire-fixtures` 校验 header size/offset、operation/status 数值、23 个 canonical request payload 长度/checksum、23 个真实 handler response 长度/checksum 和 header init | 已有 request/response corpus |
| wire schema manifest | `linqu_mem_service wire-schema` 生成 `apps/mem_service/wire-schema.txt`；`wire-schema-fixtures` 冻结 manifest length/checksum、23 个 operation、102 个字段、1 个 oneof selector | 已有最小兼容 manifest |
| artifact query binding | runtime handoff、execution artifact、training artifact query 支持 `expected_session_id`、`expected_model_key`、`expected_artifact_kind`、`expected_artifact_id`、`expected_version`、`expected_checksum` fail-closed 校验 | 已有最小上下文绑定 |
| store/journal fixture gate | `linqu_mem_service store-fixtures` 校验 metadata/ref snapshot save/load/recover，`linqu_mem_service journal-fixtures` 校验 `<store>.journal` 中 completed idempotency/audit 的 append-only replay 恢复 | 已有最小版本 |
| service process | `linqu_mem_service serve --listen unix:<path> [--store <path>]` | 已有最小 Unix-socket daemon、metadata/ref snapshot recovery 和 idempotency/audit journal recovery |
| lifecycle/admin client CLI | `linqu_mem_service health/ready/status/list-records --connect unix:<path>` | 已有最小版本 |
| object RPC | `linqu_mem_service put-object/get-object --connect unix:<path>` | 已有最小 key/value payload 版本 |
| prefix RPC | `linqu_mem_service register-prefix/lookup-prefix --connect unix:<path>` | 已有最小 key/value payload 版本 |
| KV RPC | `linqu_mem_service publish-kv/resolve-kv --connect unix:<path>` | 已有最小 key/value payload 版本 |
| runtime handoff RPC | `linqu_mem_service publish-runtime-handoff/resolve-runtime-handoff --connect unix:<path>` | 已有最小 key/value payload 版本 |
| execution artifact RPC | `linqu_mem_service register-execution-artifact/query-execution-artifact --connect unix:<path>` | 已有最小 key/value payload 版本 |
| training artifact RPC | `linqu_mem_service register-training-artifact/query-training-artifact --connect unix:<path>` | 已有最小 key/value payload 版本 |
| training-step commit CLI | `linqu_mem_service commit-training-step/resolve-training-step --connect unix:<path>` 固定 `artifact_kind=training-step-commit`，要求 session/model/artifact/version/checksum/idempotency key，默认 fail-closed | 已有最小 committed marker 版本 |
| metrics RPC/export | `linqu_mem_service metrics --connect unix:<path>` 暴露 request/status/operation hit-miss/fail-closed、idempotency replay/conflict 和 latency histogram 计数；`metrics-export --format prometheus-text` 可把同一 RPC 输出转换为 Prometheus text exposition | 已有最小观测导出版本 |
| audit-log RPC | `linqu_mem_service audit-log --connect unix:<path>` 暴露 bounded retained audit ring，覆盖 mutating operation 和 fail-closed status，记录 operation/status/request checksum/response checksum/idempotency replay/session/model/artifact/version/checksum，并随 `--store`、`<store>.journal` 和 full snapshot 持久化；runtime test 覆盖 training-step commit、stale fail-closed、metrics 计数和重启后查询 | 已有最小审计版本 |
| mutation idempotency | object/prefix/KV/runtime handoff/execution artifact/training artifact 写路径支持可选 `idempotency_key`；重复相同 operation/payload replay 首次响应，重复 key 搭配不同 payload fail-closed 为 `version_conflict`；`wire-fixtures` 覆盖进程内 replay/conflict，`store-fixtures` 覆盖 save/load replay/conflict，`journal-fixtures` 覆盖 append-only journal replay 恢复，daemon runtime test 在允许 Unix socket bind 的环境覆盖 `serve --store` 跨重启 replay/conflict | 已有最小持久化版本 |
| Qwen3 inspect CLI | `/bin/linqu_mem_service_qwen3 --inspect-qwen3` | 已有 |
| W5 Qwen3 runtime handoff | `mem_service_qwen3*.c/h` | 已有 |
| cluster/OBMM split units | `mem_service_cluster_*`, `mem_service_obmm_*` | 已有 |
| structural tests | `guest-linux/aarch64/tests/test_mem_service_record_recycling.py` | 已有 |
| independent deployment assessment | `docs/mem_service_independent_deployment_assessment.md` | 已有 |
| config/deploy contract | `serve --config <path>` 支持 text key/value config；`config-fixtures` 校验 schema 约束；发布布局包含 config schema、example config 和 systemd-like deployment manifest | 已有最小版本 |
| release manifest CLI | `linqu_mem_service release-manifest` 和 `release-fixtures` 冻结 core binary、public headers、client SDK sources、SDK examples、config/deploy artifacts、API/ABI policy artifact、metrics export format、client retry policy、pretraining refs 与 pretraining step commit client API profiles、wire/schema versions、schema manifest checksum、operation/status IDs | 已有最小版本 |
| API/ABI policy CLI | `linqu_mem_service api-abi-policy` 和 `api-abi-fixtures` 冻结 client API/ABI version、public record ABI size、wire/header/schema version、old/new policy、upgrade/rollback policy，并随 install layout 发布 | 已有最小版本 |
| admin output schema CLI | `linqu_mem_service admin-output-schema` 和 `admin-output-fixtures` 冻结当前 status/list-records/metrics/audit/snapshot/restore 文本输出、Prometheus metric prefix/type 和 fail-closed status 字段，并随 install layout 发布 | 已有最小版本 |
| compat matrix CLI | `linqu_mem_service compat-matrix`、`compat-fixtures`、`compat-baseline-v1`、`compat-baseline-fixtures`、`compat-old-new-matrix` 和 `compat-old-new-fixtures` 冻结当前 wire/schema、release layout、retry、idempotency、audit、snapshot、journal 兼容规则，old-v1-client 到 current-server 的最小兼容 baseline，以及覆盖 23 个 operation 的 v1 old/new schema-profile matrix，并随 install layout 发布 | 已有 schema-profile 版本 |
| install layout smoke | `make -C guest-linux/aarch64/apps/mem_service install-smoke DESTDIR=<dir> PREFIX=/usr` 安装 binary/header/client source/SDK examples/release manifest/wire schema manifest/admin output schema/API-ABI policy/compat matrix/v1 baseline/old-new schema-profile matrix/config/deploy/host deploy artifacts 并校验布局 | 已有最小版本 |

当前主要缺口：

| 缺口 | 对用户的影响 |
| --- | --- |
| service API 仍是最小业务路径 | 已覆盖 object/prefix/KV/runtime handoff/execution artifact/training artifact、最小 typed C client、显式 client timeout、opt-in retry/backoff、最小 mutation idempotency key、`--store`/`<store>.journal` idempotency 跨重启 replay/conflict、bounded audit-log、最小 admin、最小 metrics、Prometheus text metrics export、`deployment-fixtures` 中的 `/metrics` HTTP response envelope、`serve --metrics-listen tcp:<ipv4>:<port>` 真实 HTTP listener、collector fixture、installed-host collector smoke、请求 latency histogram、对象级 `inspect-object`、最小 `export-snapshot`、分页 `export-snapshot-page`、`export-snapshot-to` snapshot 组装、事务化分页 `restore-snapshot`、最小 restart recovery、最小 API/ABI policy、最小 release compat matrix、v1 baseline 和 old/new schema-profile matrix，但还缺 retry/idempotency old/new runtime compatibility matrix、生产级采集器/告警集成门禁和产品级 restore/durable policy |
| wire payload schema 仍是 key/value 文本 | 已有 envelope/enum/checksum/header-init fixture gate、共享 text key/value payload helper、public operation schema contract、当前 request schema fixture gate、可安装的 wire schema manifest，23 个当前 RPC 的 canonical request payload corpus 和真实 handler response corpus，以及覆盖 23 个 operation 的 old-minimal/current-plus-future schema-profile compatibility fixtures；还缺 binary/typed payload schema、旧 server runtime binary 组合测试和跨版本 compatibility fixtures |
| durable service backend 仍是最小 snapshot+journal + storage-root layout + sealed local block | 已能通过 `serve --store` 恢复 committed metadata/ref snapshot、completed idempotency record 和 retained audit event journal；`serve --config storage_root=<dir>` 已能创建 `catalog/manifest.txt`、`blocks/`、`quarantine/`，并在省略 `store` 时派生 `catalog/store.snapshot` 完成重启恢复；`payload_block_backend=sealed-local-block-v1` 已支持 `payload_inline` 和 server-side `payload_path` 文件摄取写入 `blocks/<checksum>.block`，可绕开 4096B wire payload 限制处理本地大文件 payload，object/artifact 读取时重新校验长度/checksum，损坏则 `checksum_mismatch` fail-closed 并 best-effort quarantine；还缺产品级 durable catalog、journal truncation/atomicity policy、remote/chunked sealed payload block backend、migration |
| serving client contract 仍是最小 C API | 已有 typed C client、显式 client timeout、opt-in retry/backoff/timeout retry、可选 mutation `idempotency_key`、`--store` 跨重启 replay/conflict、最小 compat matrix、old/new schema-profile matrix 和可安装 serving example，覆盖 prefix/KV/runtime handoff/execution artifact 两进程 smoke；还缺 retry/idempotency old/new runtime compatibility matrix、model/session mismatch 负例和 serving 集成矩阵 |
| pretraining object contract 已有 SDK typed wrapper，但 wire/schema 仍是最小 artifact envelope | 已有 dataset/sample/checkpoint/gradient/optimizer-state 和 `training-step-commit` pretraining helper、可安装 pretraining example、CLI commit/resolve 命令，以及外部 worker runtime test 覆盖多 worker publish/resolve、global-step committed marker、checkpoint restart、stale/checksum fail-closed、bounded audit record、append-only idempotency/audit journal 和 idempotency conflict；训练系统还缺专用 binary typed schema、产品级 multi-worker commit barrier/quorum 和产品级多 worker 一致性 |
| release/deploy contract 仍是最小布局 | 已有 release manifest CLI、源 manifest、wire schema manifest、admin output schema、API/ABI policy artifact、compat matrix、v1 baseline、old/new schema-profile matrix、config schema、example config、host daemon artifact、systemd-like deployment manifest、host systemd-like deployment manifest、`deployment-fixtures`、`admin-output-fixtures`、`api-abi-fixtures`、`host-artifact-smoke`、`installed-host-service-manager-smoke`、`durable-catalog-fixtures`、SDK examples、Prometheus text metrics export manifest entry、Prometheus metric prefix/type contract、`metrics_listen` config、`/metrics` scrape path contract、collector scrape contract、真实 HTTP listener runtime scrape 测试、portable service-manager lifecycle smoke、storage-root catalog layout manifest 和 install-smoke；还缺旧 server runtime binary 组合包、升级/回滚、真实系统 systemd 环境门禁和生产级采集器/告警门禁 |

## 3. 目标架构

目标形态分四层：

```text
mem_service core
  model-neutral record, key, object-ref, prefix, KV, tensor artifact,
  execution artifact, training artifact, durable catalog, audit

mem_service transports
  in-process C API, Unix socket RPC, TCP RPC, guest OBMM queue, host shim

mem_service adapters
  Qwen3/W5 serving adapter, future model adapters, pretraining adapter

mem_service deployment
  linqu_mem_service daemon, admin CLI, config, package, health, metrics,
  snapshot/restore, migration, release gates
```

核心原则：

1. `mem_service core` 不依赖 Qwen3、W5、QEMU、OBMM device files 或 `llm_infer`。
2. Qwen3/W5 只存在于 adapter layer。
3. OBMM 是一种 hot object transport，不是服务 API 的唯一形态。
4. Prefix/KV/execution/pretraining artifacts 必须携带 model/session/version/checksum/provenance。
5. stale 或不匹配的 object ref 必须 fail closed。
6. 独立 serving/pretraining 系统通过 client API 或 RPC 调用服务，而不是链接内部 split units。

## 4. 分阶段实现计划

### Phase 0: Baseline and Planning Freeze

目的：冻结当前状态和评估标准，避免后续把 W5 验证路径误认为独立服务。

工作项：

1. 保留 `docs/mem_service_independent_deployment_assessment.md` 作为现状判断。
2. 新增本计划文档作为后续推进基线。
3. 明确当前 completion 不能成立：daemon/RPC 已有 object/prefix/KV/runtime handoff/execution artifact/training artifact 最小业务闭环、typed C client、`--store` snapshot+journal recovery 和最小 release/install layout，但还没有 binary typed schema、产品级 durable backend、serving/pretraining 集成矩阵、配置/部署 manifest 和兼容升级门禁。

验收：

```text
docs include both assessment and implementation/evaluation plan
docs/README.md links both documents
```

### Phase 1: Core Boundary Split

目的：证明 `mem_service core` 可以脱离 `llm_infer`、Qwen3 adapter 和 W5 workload 独立存在。

实现任务：

1. 新增或整理 `mem_service_core.h`。
   - 暴露 model-neutral API。
   - 引入 `struct_size` 和 `api_version` 字段。
   - 不包含 Qwen3 naming。
2. 将 core record kinds 拆成模型无关类型：
   ```text
   PREFIX_GROUP
   PREFIX_ENTRY
   KV_SEGMENT
   TENSOR_ARTIFACT
   RUNTIME_HANDOFF
   EXECUTION_ARTIFACT
   TRAINING_ARTIFACT
   OBJECT_REF
   ```
3. 将 Qwen3 record kinds 移到 adapter namespace。
   - `MEM_SERVICE_RECORD_QWEN3_*` 不再是 core enum 的必要成员。
   - Qwen3 adapter 可以通过 adapter kind range 或 extension metadata 映射到 core record。
4. 将 `apps/mem_service` 拆成两个 build target：
   ```text
   linqu_mem_service_core
   linqu_mem_service_qwen3
   ```
   或同一 binary 下两个 link profile：
   ```text
   core-only build: no llm_infer.c
   qwen3-adapter build: links llm_infer.c
   ```
5. 让 core smoke 不依赖 `llm_infer`。
   - `linqu_mem_service --smoke`
   - `linqu_mem_service --self-test`
6. 将 Qwen3 inspect 改成 adapter-only path。
   - `linqu_mem_service_qwen3 --inspect-qwen3` 只能在 adapter build 中启用。
   - core-only build 对该命令返回明确 unsupported error。

新增测试：

1. Python layout test：
   ```text
   apps/mem_service core target must not link components/llm_infer/llm_infer.c
   qwen3 adapter target may link components/llm_infer/llm_infer.c
   ```
2. C compile smoke：
   ```text
   make -C guest-linux/aarch64/apps/mem_service linqu_mem_service_core
   make -C guest-linux/aarch64/apps/mem_service linqu_mem_service_qwen3
   ```
3. CLI smoke under guest harness:
   ```text
   run_app mem_service
   ```

验收证据：

```text
core binary builds without llm_infer.c
core smoke passes
qwen3 adapter inspect still passes
existing W5/Qwen3 tests remain green
```

### Phase 2: Stable Service API and Wire Contract

目的：把 in-process component API 变成可跨进程调用的服务 API。

实现任务：

1. 定义 `mem_service_wire.h` 或等价 schema。当前已有最小 C header。
2. 定义 request/response envelope：
   ```text
   magic
   version
   request_id
   operation
   flags
   payload_len
   payload_checksum
   status
   error_code
   ```
3. 定义最小 operation set。当前已有 operation IDs，`Health`/`Ready`、object、prefix、KV、runtime handoff、execution artifact、training artifact 已实现最小 RPC：
   ```text
   Health
   Ready
   PutObject
   GetObject
   RegisterPrefixEntry
   LookupPrefixEntry
   PublishKvSegment
   ResolveKvSegment
   PublishRuntimeHandoff
   ResolveRuntimeHandoff
   RegisterExecutionArtifact
   QueryExecutionArtifact
   ```
4. 定义稳定错误模型。当前已有 C enum 和 status name 映射：
   ```text
   OK
   NOT_FOUND
   STALE_REF
   CHECKSUM_MISMATCH
   VERSION_CONFLICT
   INVALID_MODEL_BINDING
   INVALID_SESSION
   TIMEOUT
   CAPACITY_EXCEEDED
   UNSUPPORTED
   INTERNAL
   ```
5. 定义 typed payload schema。当前 object/prefix/KV/runtime/execution/training 已有 key/value 文本 payload，CLI 和 daemon 已共享 `mem_service_wire_payload.h` 做字段读写、整数解析和 schema 校验，`mem_service_wire_schema.h` 已把当前 operation payload schema 从 daemon 私有数组提升为 public contract，`wire-schema` 已生成可安装的 `wire-schema.txt`，`wire-schema-fixtures` 已冻结 manifest length/checksum、23 个 operation、102 个字段和 1 个 oneof selector，并且 `wire-fixtures` 已冻结 23 个当前 RPC 的 canonical request payload 长度/checksum、当前 request schema 与真实 handler response 长度/checksum；runtime/execution/training artifact query 已支持 expected session/model/kind/id/version/checksum 的 fail-closed 校验；下一步要收紧为可兼容 typed schema：
   - object payload request/response。
   - prefix entry request/response。
   - KV segment request/response。
   - runtime handoff request/response。
   - execution artifact request/response。
   - training artifact request/response。
6. 定义 idempotency：
   - 当前 object/prefix/KV/runtime handoff/execution artifact/training artifact 写路径已支持可选 `idempotency_key`。
   - 相同 `idempotency_key`、相同 operation 和相同 payload 会 replay 首次响应，不重新执行 mutation。
   - 相同 `idempotency_key` 搭配不同 operation 或 payload 会 fail-closed 为 `version_conflict`。
   - 当前 `--store`、`<store>.journal` 和 full `export-snapshot` 会保存 completed idempotency record；`store-fixtures` 覆盖 save/load 后 replay/conflict，`journal-fixtures` 覆盖 append-only journal replay 恢复，daemon runtime 测试在允许 Unix socket bind 的环境覆盖 `serve --store` 重启后的 replay/conflict。
   - 当前 `audit-log` 已有 bounded retained ring，覆盖 mutating operation 和 fail-closed status，并随 `--store`、`<store>.journal` 和 full snapshot 持久化。
   - 当前 `compat-matrix` 已冻结 release-time retry/idempotency/audit/snapshot/journal 兼容规则，`compat-baseline-v1` 已冻结 old-v1-client 到 current-server 的最小 baseline，`compat-old-new-matrix` 已冻结覆盖 23 个 operation 的 v1 old/new schema-profile matrix。
   - 后续还要补 retry/idempotency old/new runtime compatibility matrix、旧 server runtime binary full old/new compatibility matrix 和产品级 journal truncation/atomicity policy。
7. 定义 compatibility rules：
   - minor version backward compatible。
   - major version requires explicit negotiation。
   - unknown fields ignored only when flagged as optional。

新增测试：

1. Wire encode/decode golden tests。
2. Request/response compatibility tests。
3. Error-code mapping tests。
4. Stale/checksum mismatch fail-closed tests。
5. daemon `Health`/`Ready` round-trip tests。
6. daemon object/prefix/KV round-trip tests。

验收证据：

```text
wire protocol has golden fixtures
old client fixture can talk to new server fixture within compatible version range
invalid refs fail closed with stable error codes
linqu_mem_service serve can answer health/ready over Unix socket
linqu_mem_service can put/get object and publish/resolve prefix/KV over Unix socket
```

### Phase 3: Daemon and Admin CLI

目的：让 `mem_service` 成为可独立部署的长期运行服务。

实现任务：

1. 增加 daemon command：
   ```text
   linqu_mem_service serve --listen unix:<path>     # current minimal path
   linqu_mem_service serve --listen unix:<path> --store <path>
   linqu_mem_service serve --config <path>          # current minimal path
   linqu_mem_service serve --listen tcp:<addr>
   ```
2. 增加 admin commands：
   ```text
   linqu_mem_service health --connect unix:<path>   # current minimal path
   linqu_mem_service ready --connect unix:<path>    # current minimal path
   linqu_mem_service status                         # current minimal path
   linqu_mem_service list-records                   # current minimal path
   linqu_mem_service metrics                        # current minimal path
   linqu_mem_service metrics-export --format prometheus-text  # current minimal exporter path
   linqu_mem_service inspect-object <key>           # current minimal path
   linqu_mem_service export-snapshot                # current minimal path, bounded by wire payload size
   linqu_mem_service export-snapshot-page           # current minimal path, paginated by record slot
   linqu_mem_service export-snapshot-to --to <path> # current minimal page assembly path
   linqu_mem_service restore-snapshot <path>        # current transactional paged restore for large snapshots
   ```
3. 增加 service lifecycle：
   ```text
   startup
   config load
   backend open
   recovery
   ready
   graceful shutdown
   flush
   ```
4. 增加 observability：
   ```text
   request count                         # current minimal metrics RPC
   error count by code                   # current minimal metrics RPC
   object count                          # current status RPC
   prefix hit/miss count                 # current minimal metrics RPC
   KV publish/resolve count              # current minimal metrics RPC
   stale fail-closed count               # current minimal metrics RPC
   latency histogram                     # current minimal fixed buckets
   prometheus text export                 # current minimal CLI exporter
   backend bytes
   queue depth
   ```
5. 增加 config schema：
   ```text
   listen              # current minimal required field
   store               # current minimal optional field
   node_id             # current schema-only field
   cluster_id          # current schema-only field
   storage_root        # current storage-root catalog layout and derived store field
   backend             # current schema validates snapshot and snapshot+journal
   max_records         # current schema validates u64
   max_payload_bytes   # current schema validates u64
   retention           # current schema-only field
   auth mode           # current schema validates none
   metrics mode        # current schema validates text-kv
   adapter enablement  # current schema validates core/qwen3
   ```

新增测试：

1. Daemon start/stop smoke。
2. Health/ready probe。
3. Admin CLI round trip。
4. Concurrent client smoke。
5. Graceful shutdown flush check。
6. Restart reloads committed metadata/ref snapshot。

验收证据：

```text
daemon can run without W5/Qwen3 harness
client can put/get object through RPC
admin CLI can inspect state
service exits cleanly and restarts
service reloads committed metadata/ref records from --store
```

### Phase 4: Durable Backend

目的：满足独立服务的 restart/recovery 和发布部署要求。

实现任务：

1. 定义 durable catalog：
   ```text
   records
   object refs
   prefix entries
   KV segment manifests
   execution artifacts
   training artifacts
   index metadata
   ```
2. 定义 payload block backend：
   ```text
   inline small payload
   block-backed medium/large payload
   checksum-addressed blocks
   sealed blocks
   dedupe
   ```
3. 定义 audit log：
   ```text
   append-only events
   event checksum chain
   actor/source
   request id
   operation
   before/after version
   error result
   ```
4. 定义 snapshot/restore：
   ```text
   consistent catalog snapshot
   payload block reference validation
   restore version
   migration version
   ```
5. 定义 conflict and consistency：
   ```text
   compare-and-swap version update
   monotonic object version
   committed refs immutable by checksum
   stale refs fail closed
   ```

新增测试：

1. Publish, restart, resolve。
2. Snapshot, restore, resolve。
3. Corrupt block fails closed。
4. Stale version fails closed。
5. Concurrent version conflict deterministic。
6. Audit replay reconstructs final catalog。

验收证据：

```text
committed prefix/KV/object refs survive restart
corruption and stale refs never silently return payload
audit log can explain a state transition
```

### Phase 5: LLM Serving Integration

目的：让外部 LLM serving 使用独立 `mem_service`。

实现任务：

1. 定义 serving session namespace：
   ```text
   tenant
   model_id
   model_revision
   tokenizer_revision
   session_id
   request_id
   decode_step
   ```
2. 定义 prefix cache contract：
   ```text
   prefix_hash
   token_range
   model binding
   tokenizer binding
   KV refs
   hidden refs
   confidence
   verification state
   ```
3. 定义 KV segment contract：
   ```text
   layer range
   head range
   position range
   dtype
   shape
   object ref
   checksum
   version
   GSVA/GVA metadata when available
   ```
4. 定义 runtime handoff contract：
   ```text
   input tensor refs
   output tensor refs
   range owner
   downstream owner
   publish timestamp
   producer clock metadata
   ```
5. 定义 client API：
   ```text
   mem_client_register_prefix
   mem_client_lookup_prefix
   mem_client_publish_kv
   mem_client_resolve_kv
   mem_client_publish_handoff
   mem_client_resolve_handoff
   ```
   当前已有 `apps/mem_service/examples/mem_service_serving_example.c` 作为
   最小 SDK 示例，覆盖 prefix/KV/runtime handoff/execution artifact
   两进程 publish/query smoke，并显式配置 opt-in retry/backoff controls 和
   mutation `idempotency_key`。下一步仍需要 release-grade SDK API、
   retry/idempotency old/new runtime compatibility matrix、durable idempotency、模型/会话绑定负例和
   serving 集成矩阵。
6. 保持 Qwen3 adapter 作为 first serving adapter，但不让 Qwen3 类型进入 core API。

新增测试：

1. Two-process serving simulation：
   ```text
   server process publishes prefix/KV
   client process resolves prefix/KV
   ```
2. Stale session fails closed。
3. Model mismatch fails closed。
4. Checksum mismatch fails closed。
5. Prefix hit/miss metrics。
6. W5 4-step regression still passes through adapter.

验收证据：

```text
serving process can integrate without linking mem_service internals
prefix/KV reuse works through RPC
adapter W5 path remains functional
```

### Phase 6: Pretraining Integration

目的：让 pretraining worker 使用 `mem_service` 作为训练协同 metadata service。

实现任务：

1. 定义 training namespace：
   ```text
   job_id
   run_id
   worker_id
   global_step
   microbatch_id
   data_epoch
   checkpoint_id
   ```
2. 定义 training artifact kinds：
   ```text
   dataset_shard_ref
   sample_batch_ref
   tokenized_sequence_ref
   activation_checkpoint_ref
   gradient_bucket_ref
   optimizer_state_ref
   model_checkpoint_ref
   training_step_commit_ref
   scheduler_barrier_ref
   ```
3. 定义 multi-writer semantics：
   ```text
   worker-owned refs
   minimal global-step committed marker exists via training-step-commit
   release-grade global-step commit barrier/quorum
   conflict deterministic
   immutable payload refs
   mutable index records via version CAS
   ```
4. 定义 training audit：
   ```text
   samples consumed
   checkpoints produced
   gradient buckets published
   optimizer state versions
   restart source
   ```
5. 增加 pretraining client API：
   ```text
   publish_dataset_shard
   resolve_dataset_shard
   publish_sample_batch
   publish_activation_checkpoint
   publish_gradient_bucket
   publish_optimizer_state
   commit_training_step
   resolve_training_step
   resolve_checkpoint
   ```
   当前已有 `apps/mem_service/examples/mem_service_pretraining_example.c`
   作为最小 SDK 示例，使用 `publish_dataset_shard`、
   `publish_sample_batch`、`publish_checkpoint`、
   `publish_gradient_bucket`、`publish_optimizer_state`、`commit_training_step`
   及对应 resolve helper
   覆盖 dataset shard、sample batch、checkpoint、gradient bucket、optimizer
   state、training-step commit marker 的两进程 publish/query smoke。当前
   `test_pretraining_workers_publish_resolve_and_recover_refs` 还会编译一个
   外部 pretraining worker client，覆盖 worker0/worker1 多进程
   publish、typed resolve、global-step committed marker、checkpoint restart
   recovery、stale/checksum fail-closed 和 idempotency conflict；
   `test_cli_training_step_commit_barrier_round_trips_fail_closed` 覆盖
   CLI commit/resolve/stale/conflict。下一步仍需要把 SDK typed helper
   背后的 text artifact envelope 提升为发布级 typed schema，并补
   release-grade global-step commit barrier/quorum、audit/replay records
   和产品级多 worker 一致性。

新增测试：

1. Multi-worker publish/resolve simulation。
2. Restart after checkpoint publish。
3. Conflict on duplicate global step。
4. Audit reconstructs training step。
5. Corrupt payload block fails closed。

验收证据：

```text
pretraining workers can coordinate through mem_service metadata
restart does not lose committed checkpoint refs
audit can reconstruct a training step's inputs and outputs
```

### Phase 7: Release and Deployment

目的：形成可发布、可部署、可升级的 service。

实现任务：

1. 定义 release artifacts：
   ```text
   linqu_mem_service
   libmem_service_client
   mem_service headers
   config schema
   protocol schema/golden fixtures
   admin output schema
   metrics export format
   deployment examples
   host daemon artifact
   host deployment manifest
   ```
   当前已有最小 manifest：`apps/mem_service/release-manifest.txt` 和
   `linqu_mem_service release-manifest`，冻结 core binary、public headers、
   client SDK sources、wire/schema versions、wire schema manifest checksum、
   admin output schema checksum、config/deploy artifacts、Prometheus text metrics export format、explicit
   client retry policy、operation IDs、status IDs、host daemon artifact 和
   host deployment manifest。
2. 定义 install layout：
   ```text
   bin/
   include/
   lib/
   share/mem_service/schema/
   share/mem_service/examples/
   ```
   当前已有最小 install smoke：
   `make -C guest-linux/aarch64/apps/mem_service install-smoke DESTDIR=<dir>
   PREFIX=/usr`，安装 core binary、public headers、client SDK source、
   SDK examples、release manifest、wire schema manifest、config schema/example、
   deployment manifest、host daemon artifact 和 host deployment manifest。
3. 定义 deployment manifests：
   ```text
   guest initramfs entry
   host daemon example
   systemd-like service file for host simulation
   container-like rootfs layout if needed
   ```
4. 定义 upgrade policy：
   ```text
   protocol major/minor
   catalog schema version
   snapshot migration
   downgrade restrictions
   ```
5. 定义 release gates：
   ```text
   all unit tests
   all guest contract tests
   daemon smoke
   RPC golden compatibility
   durability restart
   serving integration
   pretraining integration
   metrics export/audit check
   ```

验收证据：

```text
release artifact can be built from clean checkout
deployment can start service and pass health/ready
client can run serving and pretraining smoke against deployed service
```

## 5. 评估计划

### 5.1 Readiness Levels

| Level | 名称 | 判定 |
| --- | --- | --- |
| L0 | Component only | 只能 link-time 使用 |
| L1 | Core independent | core 不依赖 `llm_infer`/Qwen3 |
| L2 | Local service | 有 daemon 和 local IPC |
| L3 | Durable service | restart/recovery 可验证 |
| L4 | Serving ready | 外部 LLM serving 可通过 client/RPC 协同 |
| L5 | Pretraining ready | training workers 可通过 service 协同 |
| L6 | Release ready | 有 artifact、install、config、compat、release gates |

当前评估：`L4-minimal`。

理由：

```text
component and guest app exist
core app now builds without llm_infer, qwen3 adapter sources, or the guest runtime aggregate unit
mem_service_core.h exists as the model-neutral core include surface
qwen3 inspect is isolated in a qwen3 adapter binary
qwen3 record names are adapter aliases, not core enum names
linqu_mem_service serve can run as a Unix-socket service process
linqu_mem_service health/ready can call the service over the wire envelope
business RPC operations are encoded for object/prefix/KV/runtime/execution/training minimal paths
no binary typed object/prefix/KV/pretraining payload schema
lightweight Unix RPC client transport exists without linking daemon/server/core
typed C client wrappers exist for object/prefix/KV/runtime/execution/training RPC and pretraining dataset/sample/checkpoint/gradient/optimizer/training-step commit helpers without linking daemon/server/core
installable serving/pretraining SDK examples prove minimal two-process RPC smoke against the daemon
pretraining worker runtime gate proves typed multi-worker publish/resolve, checkpoint restart, stale/checksum fail-closed, and idempotency conflict
shared text key/value payload helper, public operation schema contract, and request schema fixture gate exist
wire-schema CLI and checked-in wire-schema.txt freeze the current operation/field manifest
minimal --store snapshot+journal exists and carries completed idempotency records plus bounded audit records; storage_root layout manifest, derived store recovery, and sealed-local-block-v1 inline/file payload write/verify exist, but no production durable catalog/remote chunked sealed block backend or journal truncation/atomicity policy
minimal release manifest CLI, wire schema manifest, compat matrix, v1 baseline, old/new schema-profile matrix, config/deploy artifacts, SDK examples, and install-smoke layout exist
no retry/idempotency old/new runtime compatibility matrix, model/session mismatch matrix, or product-grade multi-worker pretraining commit/audit matrix
```

### 5.2 Gate Matrix

| Gate | 证明内容 | 证据类型 |
| --- | --- | --- |
| G1 Core split | core build does not link `llm_infer.c` | Makefile + build log + test |
| G2 Adapter isolation | Qwen3 works only through adapter | tests + public header inspection |
| G3 Service process | daemon starts, ready, handles shutdown | CLI integration test |
| G4 Wire API | request/response stable | golden fixtures + compatibility tests |
| G5 Durability | restart preserves committed refs | restart tests |
| G6 Fail-closed | stale/checksum/model mismatch rejects | negative tests |
| G7 Serving | external serving client uses prefix/KV/object refs | two-process integration |
| G8 Pretraining | workers publish/resolve training artifacts | multi-worker integration |
| G9 Release | artifact install and deployment pass smoke | release script + deployment smoke |

### 5.3 Functional Test Plan

Required current tests that must remain green after every phase:

```bash
cargo test --workspace
python3 -m unittest discover guest-linux/aarch64/tests
cargo fmt --all -- --check
git diff --check
```

New tests by phase:

```text
Phase 1:
  test_mem_service_core_build_without_llm_infer
  test_mem_service_qwen3_adapter_build_with_llm_infer
  test_mem_service_core_cli_smoke

Phase 2:
  test_mem_service_wire_golden_roundtrip
  test_mem_service_wire_schema_manifest
  test_mem_service_wire_version_compat
  test_mem_service_wire_error_model
  test_mem_service_daemon_health_ready
  test_mem_service_daemon_object_prefix_kv_roundtrip

Phase 3:
  test_mem_service_daemon_graceful_shutdown
  linqu_mem_service store-fixtures
  linqu_mem_service journal-fixtures
  test_daemon_store_survives_restart_for_object_refs

Phase 4:
  test_mem_service_restart_preserves_prefix_kv_refs
  test_mem_service_corrupt_payload_fails_closed
  test_mem_service_audit_replay

Phase 5:
  test_mem_service_serving_sdk_example_round_trip
  test_mem_service_serving_prefix_kv_two_process
  test_mem_service_serving_model_mismatch_fails_closed
  test_mem_service_w5_adapter_regression_4step

Phase 6:
  test_mem_service_pretraining_sdk_example_round_trip
  test_pretraining_workers_publish_resolve_and_recover_refs
  test_mem_service_pretraining_commit_barrier
  test_mem_service_pretraining_audit_replay

Phase 7:
  test_mem_service_release_artifact_layout
  test_mem_service_install_smoke
  linqu_mem_service wire-schema-fixtures
  linqu_mem_service release-fixtures
  test_mem_service_protocol_compatibility_bundle
```

### 5.4 Performance Evaluation

Metrics:

```text
RPC request latency p50/p95/p99
PutObject throughput
GetObject throughput
Prefix lookup latency
KV segment publish latency
KV segment resolve latency
Restart recovery time
Snapshot time
Audit append overhead
Serving token-path overhead
Pretraining step barrier overhead
```

Baselines:

```text
in-process C API
Unix socket RPC
TCP RPC
guest OBMM queue path
durable backend enabled
durable backend disabled
```

Required performance reports:

```text
host local microbench
guest local microbench
two-process serving benchmark
multi-worker pretraining metadata benchmark
restart/recovery benchmark
```

Performance acceptance must not claim model-level speedup unless the benchmark covers model execution. Metadata service benchmarks only prove service overhead and data-plane/control-plane behavior.

### 5.5 Safety and Correctness Evaluation

Fail-closed cases:

```text
stale version
wrong model binding
wrong tokenizer binding
wrong session id
checksum mismatch
shape mismatch
dtype mismatch
missing payload block
corrupt payload block
expired record
quarantined record
unsupported protocol version
partial write
crash during commit
```

Correctness evidence:

```text
negative tests assert explicit error codes
audit log records rejected operations
no fallback returns unrelated payload
restart preserves committed state and drops uncommitted partial state
```

### 5.6 Compatibility Evaluation

Artifacts to freeze per release:

```text
public headers
wire protocol schema
wire schema manifest
golden fixtures
config schema
snapshot schema
error code table
metric names
admin CLI output contract
release manifest
install layout
```

Compatibility policy:

```text
patch release: no schema change
minor release: additive optional fields only
major release: explicit protocol negotiation and migration required
```

## 6. Work Breakdown for Next Iterations

Recommended order:

1. Add old/new compatibility fixtures for `Health`/`Ready`/`Status`/
   `ListRecords`, object, prefix, KV, runtime handoff, execution artifact, and
   training artifact. The current `wire-fixtures` gate already freezes the
   envelope, operation/status values, checksum algorithm, header init behavior,
   23 canonical request payloads, and 23 real handler responses. The current
   `wire-schema` manifest freezes the present operation/field surface, the
   current `compat-matrix` freezes release-time compatibility rules, and
   `compat-baseline-v1` freezes the current old-v1-client baseline, and
   `compat-old-new-matrix` freezes the current v1 old/new schema-profile matrix
   for all 23 operations. This is still not a full old-server runtime-binary
   client/server compatibility matrix.
2. Promote the shared text key/value payload helper and public operation schema
   contract into a release-grade typed schema layer. The current helper removes
   CLI/daemon parser drift and validates present request fields, but it is not
   yet a binary typed schema or a
   cross-version compatibility contract.
3. Add CLI/daemon golden round-trip tests for `PublishRuntimeHandoff`,
   `ResolveRuntimeHandoff`, `RegisterExecutionArtifact`,
   `QueryExecutionArtifact`, `RegisterTrainingArtifact`, and
   `QueryTrainingArtifact`.
4. Extend graceful shutdown, product-grade restore policy, and deploy-grade
   metrics collection gates.
   The current `status`/`list-records`/`metrics`/
   `inspect-object`/`export-snapshot`/`export-snapshot-page`/
   `export-snapshot-to`/`restore-snapshot`/`metrics-export` commands are the
   minimal admin slice, including page assembly, transactional paged restore,
   and Prometheus text export.
5. Extend the current storage-root catalog layout into a product durable
   catalog after RPC contracts are stable enough to test.
6. Promote the current `mem_service_client.c/h` typed C API into a
   release-grade serving/pretraining SDK:
   stable headers, retry/idempotency compatibility fixtures, package metadata,
   and expanded examples. The current typed client already exposes explicit
   timeout and opt-in retry/backoff controls, and the current installable
   serving/pretraining examples already link only `mem_service_client.c` plus
   `mem_service_wire_client.c`, configure retry/backoff explicitly, carry
   idempotency keys for mutating calls, and pass a daemon two-process smoke.
7. Promote the current SDK-level pretraining dataset/sample/checkpoint,
   gradient, and optimizer-state helpers into release-grade typed wire/schema
   contracts.
8. Extend the minimal release artifact layout into deployable package gates:
   the current config schema, example config, service unit manifest, API/ABI
   policy artifact, `/metrics` response envelope, and TCP metrics listener contract are installed and
   fixture-checked, a host daemon artifact is installed under `libexec`,
   checked by `host-artifact-smoke`, and exercised through the installed host
   service unit by `installed-host-service-manager-smoke`; a portable lifecycle
   smoke now covers config startup, ready/health, HTTP scrape, collector
   metrics parse, SIGTERM stop, and socket cleanup. The current
   `admin-output-schema` artifact freezes metric names, Prometheus metric
   prefix/type, and admin CLI text-output fields for the minimal admin slice.
   Remaining work is protocol
   compatibility bundle, upgrade/rollback smoke, real systemd environment
   smoke, production collector/alert integration smoke, and old/new runtime
   compatibility coverage for admin/metrics outputs.

## 7. Completion Definition

The full goal is complete only when all of these are proven in the current tree:

1. `mem_service` core builds and self-tests without Qwen3 or `llm_infer`.
2. Qwen3/W5 integration works as adapter, not service core.
3. `linqu_mem_service serve` runs as an independent process.
4. External clients can call it through a stable RPC/wire API.
5. Prefix/KV/object/runtime handoff operations work through that API.
6. Durable metadata and payload refs survive restart.
7. stale/checksum/model/session mismatches fail closed.
8. LLM serving integration is proven by a two-process test.
9. Pretraining integration is proven by a multi-worker artifact test.
10. Release artifact layout, config schema, deployment smoke, and compatibility fixtures exist.
11. Full repo validation passes:
    ```bash
    cargo test --workspace
    python3 -m unittest discover guest-linux/aarch64/tests
    cargo fmt --all -- --check
    git diff --check
    ```

Until every item above has direct evidence, the goal remains incomplete.
