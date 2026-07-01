# mem_service 实现总结

更新时间：2026-07-01

## 1. 结论

`mem_service` 当前已经不是嵌在 `llm_infer` 里的辅助函数，而是一个具备独立二进制、Unix-socket daemon、C client SDK、安装布局、打包门禁、release readiness 自描述、serving/pretraining 消费样例和 W5/Qwen3 runtime adapter 的 Memory Service。

当前可以成立的判断：

- **可以独立构建和本地独立运行**：`linqu_mem_service serve` 可以作为 Unix-socket 服务进程启动，外部客户端通过 wire/client SDK 调用。
- **可以被 `llm_infer` 端到端消费**：`llm_infer` 已有 mem-service publish/verify 路径，覆盖 serving 侧 prefix、KV、runtime handoff 和 execution artifact。
- **可以被 pretraining 客户端消费**：新增 `pretraining_client`，覆盖 dataset shard、sample batch、checkpoint、gradient bucket、optimizer state 和 training step commit。
- **可以生成发布包并做本地包内复验**：tar/deb smoke、installed SDK smoke、installed layout selfcheck、release/package manifest 都已经具备。
- **还不能宣称生产部署完全认证**：真实 Linux systemd、rpm toolchain、Prometheus/Alertmanager、跨主机 remote transport 的证据仍需要在真实 Linux/多机环境里跑出并复验。

所以更准确的状态是：**实现层面已经接近独立服务形态；生产级独立发布部署还缺真实环境认证证据。**

## 2. 代码边界

主要目录：

```text
guest-linux/aarch64/components/mem_service/
guest-linux/aarch64/apps/mem_service/
guest-linux/aarch64/apps/llm_infer/
guest-linux/aarch64/apps/pretraining_client/
guest-linux/aarch64/scripts/
guest-linux/aarch64/tests/
```

核心分层：

| 层 | 代表文件 | 职责 |
| --- | --- | --- |
| Core metadata/object service | `mem_service.c`, `mem_service_metadata.c`, `mem_service_records.c`, `mem_service_keys.c` | record/object/prefix/KV 元数据、key 构造、记录表管理 |
| Wire daemon | `mem_service_daemon.c`, `mem_service_daemon.h`, `mem_service_wire.h` | Unix-socket 服务循环、请求/响应 envelope、operation dispatch |
| Client SDK | `mem_service_client.c`, `mem_service_client.h`, `mem_service_wire_client.c`, `mem_service_wire_client.h` | 外部 serving/pretraining 进程可链接的 typed C client |
| Wire payload/schema | `mem_service_wire_payload.h`, `mem_service_wire_schema.h`, `wire-schema.txt` | text-kv payload、schema 校验、operation 字段契约 |
| Qwen3/W5 adapter | `mem_service_qwen3*.c`, `mem_service_qwen3*.h` | Qwen3 runtime range、KV、terminal token、engram、decode barrier |
| Cluster/OBMM runtime | `mem_service_cluster_*.c`, `mem_service_guest_runtime.h`, `mem_service_object_contract.h` | OBMM/cluster payload、slot、queue、object ref 协同 |
| App/packaging | `apps/mem_service/Makefile`, `configs/`, `deploy/`, `packaging/` | 构建、安装、systemd、rpm/deb/tar、release gates |
| Consumers | `apps/llm_infer/`, `apps/pretraining_client/` | 实际 serving/pretraining 消费方验证入口 |

## 3. 二进制和 CLI

当前 `apps/mem_service` 构建这些二进制：

```text
linqu_mem_service
linqu_mem_service_core
linqu_mem_service_qwen3
linqu_mem_service_host
```

主要 CLI 能力：

```text
linqu_mem_service serve --config <path>
linqu_mem_service serve --listen unix:<path> --store <path>
linqu_mem_service health --connect unix:<path>
linqu_mem_service ready --connect unix:<path>
linqu_mem_service status --connect unix:<path>
linqu_mem_service list-records --connect unix:<path>
linqu_mem_service metrics --connect unix:<path>
linqu_mem_service metrics-export --format prometheus-text
linqu_mem_service audit-log --connect unix:<path>
```

业务对象 CLI：

```text
put-object / get-object / inspect-object
register-prefix / lookup-prefix
publish-kv / resolve-kv
publish-runtime-handoff / resolve-runtime-handoff
register-execution-artifact / query-execution-artifact
register-training-artifact / query-training-artifact
commit-training-step / resolve-training-step
```

发布和契约 CLI：

```text
version
release-readiness
release-manifest
package-manifest
wire-schema
admin-output-schema
api-abi-policy
compat-matrix
compat-baseline-v1
compat-old-new-matrix
upgrade-rollback-policy
ops-certification-policy
alert-rules
```

fixture/gate CLI 覆盖：

```text
wire-fixtures
wire-schema-fixtures
store-fixtures
journal-fixtures
journal-compaction-fixtures
journal-torn-recovery-fixtures
config-fixtures
metrics-export-fixtures
collector-fixtures
alert-fixtures
alert-integration-fixtures
ops-certification-fixtures
ops-certification-evidence-fixtures
deployment-fixtures
admin-output-fixtures
upgrade-rollback-fixtures
upgrade-rollback-runtime-fixtures
restore-policy-fixtures
runtime-quota-fixtures
retention-fixtures
checkpoint-retention-fixtures
payload-gc-fixtures
record-retention-fixtures
durable-catalog-fixtures
chunked-block-fixtures
transport-block-fixtures
network-transport-block-fixtures
remote-block-backend-policy-fixtures
remote-transport-evidence-fixtures
client-retry-fixtures
api-abi-fixtures
compat-fixtures
compat-baseline-fixtures
compat-old-new-fixtures
compat-runtime-fixtures
compat-old-server-runtime-fixtures
serving-fail-closed-fixtures
pretraining-fail-closed-fixtures
typed-payload-fixtures
package-fixtures
release-fixtures
release-readiness-fixtures
```

这些 fixture 的价值是：把能力变成可执行门禁，而不是只写在文档里。

## 4. Wire 协议实现

`mem_service_wire.h` 定义稳定 request/response envelope：

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

当前服务端以 Unix socket 提供 RPC。payload 现阶段默认是 text key/value，并已经有 schema manifest 固化字段、类型和 selector 规则。

已支持的 operation 覆盖：

- health/ready/status/list-records
- metrics/audit/snapshot/restore
- object put/get/inspect
- prefix register/lookup
- KV publish/resolve
- runtime handoff publish/resolve
- execution artifact register/query
- training artifact register/query
- training step commit/resolve

同时已经加入 typed binary payload 演进路径：

- magic/version/field count
- TLV 字段编码
- string/u32/u64 round-trip
- future version reject
- truncated/bad magic fail-closed

当前 text-kv 仍是兼容默认路径，typed-binary 是已经验证过的演进入口，还没有替代全部业务 payload。

## 5. fail-closed 语义

这里的 fail-closed 含义是：**当引用、上下文、版本或 checksum 不匹配时，请求失败并返回明确错误，不隐式切到本地路径、不假装命中、不悄悄复算。**

已经实现的校验维度：

- `expected_session_id`
- `expected_model_key`
- `expected_artifact_kind`
- `expected_artifact_id`
- `expected_owner`
- `expected_version`
- `expected_checksum`

典型错误状态：

- `invalid_session`
- `invalid_model_binding`
- `stale_ref`
- `checksum_mismatch`
- `version_conflict`

serving 和 pretraining 都有负例矩阵：

- `serving-fail-closed-fixtures`
- `pretraining-fail-closed-fixtures`

这部分的意义是防止 prefix/KV/runtime/training artifact 被错误复用。对于 LLM serving 来说，错误命中比 miss 更危险；miss 可以退化为重新生成，错误命中会污染输出。

## 6. Serving 协同能力

`llm_infer` 已接入 mem_service 的 socket API，形成可选的 mem-service-backed serving 路径。

消费入口：

```text
guest-linux/aarch64/apps/llm_infer/llm_infer.c
guest-linux/aarch64/apps/llm_infer/Makefile
```

能力范围：

- prefix publish/lookup
- KV segment publish/resolve
- runtime handoff publish/resolve
- execution artifact register/query
- daemon restart 后 verify

典型模式：

```text
--mem-service-serving-publish unix:<path>
--mem-service-serving-verify unix:<path>
```

这条路径验证的是：`llm_infer` 作为外部 consumer，不需要把 mem_service 静态当成本地内部函数才能消费服务。它可以通过 socket 发布和查询 serving 侧对象。

当前边界：

- 它证明的是 `llm_infer` 与 mem_service 的端到端协同。
- 它不等价于已经完成生产多机服务编排。
- 它也不自动证明性能收益；性能收益还要看 prefix/KV 命中后上游 decode 计算是否真的减少，以及数据面是否走 GVA/GSVA 快路径。

## 7. Pretraining 协同能力

新增正式 app：

```text
guest-linux/aarch64/apps/pretraining_client/
```

二进制：

```text
linqu_pretraining_client
```

CLI 模式：

```text
linqu_pretraining_client --mem-service-pretraining-publish unix:<path>
linqu_pretraining_client --mem-service-pretraining-verify unix:<path>
```

覆盖对象：

- dataset shard
- sample batch
- checkpoint
- gradient bucket
- optimizer state
- training step commit

实现方式：

- `pretraining_client` 链接 `mem_service_client.c` 和 `mem_service_wire_client.c`
- publish 阶段写入一组 training refs
- verify 阶段用 expected context/version/checksum 查询
- daemon restart 后仍能通过 store 恢复并查询成功

这条路径的价值是：mem_service 不再只是 LLM inference cache 服务，也具备训练侧 artifact/ref registry 的最小服务形态。

当前边界：

- 现在是 pretraining artifact/ref 协同验证。
- 还不是完整训练系统的数据调度、checkpoint 生命周期管理或多 worker 一致性服务。

## 8. Qwen3/W5 Runtime Adapter

`components/mem_service/mem_service_qwen3*.c` 是 Qwen3/W5 adapter 层，负责把通用 mem_service 能力接到当前 W5 runtime。

已经覆盖的 W5/Qwen3 runtime 对象：

- runtime range input wait/view
- scheduler work item wait
- range output publish
- KV state publish/resolve
- terminal token publish/wait
- shortpath terminal publish
- engram candidate publish/wait
- engram selected token wait
- engram history/state wait
- decode round barrier
- Qwen3 layer range placement
- Qwen3 KV span allocation
- Qwen3 object key construction

这些能力服务当前 W5 decode runtime 的对象 handoff，包括 hidden range、KV state、terminal token、engram state 和 decode barrier。

边界需要说清楚：

- mem_service 负责 metadata、object refs、schema、校验、持久化和服务化访问。
- GVA/GSVA 的数据面收益取决于底层 OBMM/cluster runtime 是否让 payload/ref 实际走 GVA/GSVA-backed path。
- mem_service 自身不是矩阵计算引擎，也不是 decode scheduler；它是 runtime object/cache/artifact service。

## 9. 持久化和恢复

当前持久化不是单纯 JSON 文件堆积，已经包含 snapshot、journal、schema、compaction 和 restore policy。

已实现能力：

- `storage_root` catalog
- `store_schema_version=1`
- legacy 无版本 store snapshot 迁移到 v1
- future/malformed schema fail-closed
- snapshot export/import
- paged snapshot export
- transactional staged restore
- bad magic restore 拒绝
- out-of-order page 拒绝
- record-count mismatch 拒绝
- cancelled stage commit 拒绝
- journal append `fflush` + `fsync`
- torn trailing frame recovery
- threshold compaction
- idempotency replay/conflict

对应 gate：

```text
store-fixtures
journal-fixtures
journal-compaction-fixtures
journal-torn-recovery-fixtures
restore-policy-fixtures
```

当前边界：

- 已具备本地 durable service 的基本工程形态。
- 还不是分布式强一致 catalog。
- 还没有生产级 HA、leader election、多副本复制、跨机一致性恢复。

## 10. Payload Backend

已实现 payload/backend 形态：

- inline payload
- payload file
- sealed local block
- sealed chunked block
- transport loopback block
- TCP loopback block

chunked backend：

- 大 payload 按固定 chunk 写入
- manifest 记录 chunk count、chunk size、total len、checksum
- validate 时重组并重算 checksum
- mismatch 后 fail-closed quarantine

remote/transport backend：

- loopback transport 用于验证 transport backend 框架
- TCP loopback 用于验证网络 fetch/write/validate 流程
- production remote transport 通过 evidence 文件和 bundle verifier 保持 fail-closed

当前边界：

- loopback/TCP-loopback 证明了数据面机制。
- 真正跨主机 producer/consumer 分离、网络分区标记、非 loopback source 的生产 remote transport 还需要真实多机 CI 证据。

## 11. 配置、安全边界和运维

配置文件：

```text
guest-linux/aarch64/apps/mem_service/configs/mem_service.conf.schema
guest-linux/aarch64/apps/mem_service/configs/mem_service.example.conf
guest-linux/aarch64/apps/mem_service/configs/mem_service.runtime.conf
guest-linux/aarch64/apps/mem_service/configs/mem_service.host.runtime.conf
```

部署文件：

```text
guest-linux/aarch64/apps/mem_service/deploy/linqu_mem_service.service
guest-linux/aarch64/apps/mem_service/deploy/linqu_mem_service.host.service
guest-linux/aarch64/apps/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml
```

当前安全边界：

- `auth_mode=none` 只允许 Unix socket service endpoint
- metrics 只允许 `tcp:127.0.0.1:<port>`
- 非 loopback metrics endpoint fail-closed
- quota/retention/encryption config 有 parser 和 fixture gate

观测能力：

- metrics endpoint
- Prometheus text export
- audit log
- collector fixture
- alert rule fixture
- alert integration fixture

当前边界：

- 这是 local-only/no-auth 的部署安全边界。
- 还没有 TLS/mTLS、ACL、多租户隔离、远端认证授权。
- Prometheus/Alertmanager 真实生产联调还需要 Linux CI/ops evidence。

## 12. 发布、安装和包

Makefile 发布相关目标：

```text
install
install-smoke
installed-sdk-example-smoke
installed-sdk-pkgconfig-smoke
installed-sdk-runtime-smoke
package-tarball
package-tarball-smoke
package-deb
package-deb-smoke
package-rpm
package-rpm-smoke
host-artifact-smoke
```

安装布局包含：

- `/usr/bin/linqu_mem_service`
- `/usr/libexec/lingqu/mem_service/linqu_mem_service_host`
- public headers
- SDK sources
- serving/pretraining examples
- `pkg-config` metadata
- config schema/example/runtime config
- systemd unit
- Prometheus alert rules
- release/package/wire/admin/API/compat manifests
- release verification scripts

发布脚本随包安装到：

```text
share/lingqu/mem_service/scripts/
```

代表脚本：

```text
verify_mem_service_installed_layout.sh
verify_mem_service_installed_sdk.sh
run_mem_service_linux_ops_ci.sh
verify_mem_service_linux_ops_evidence.sh
verify_mem_service_ops_certification_bundle.sh
run_mem_service_remote_transport_ci.sh
verify_mem_service_remote_transport_evidence.sh
verify_mem_service_remote_transport_bundle.sh
verify_mem_service_release_certification.sh
run_mem_service_release_certification_ci.sh
```

当前状态：

- tar/deb 本地 smoke 已具备。
- rpm target 和 smoke 已具备，但真实 rpm 验收需要 Linux rpm toolchain。
- installed SDK 可以通过 `pkg-config` 被外部项目发现和消费。
- installed runtime smoke 会启动安装后的 daemon 并运行 serving/pretraining example。

## 13. Release Readiness

`release-readiness` 是当前 mem_service 发布放行状态的机器可读入口。

默认状态会在缺少外部证据时保持 not-certified。只有同时提供并通过以下证据后，才能输出 certified：

- Linux ops evidence
- remote transport evidence

相关入口：

```text
release-readiness --ops-evidence-file <path> --remote-transport-evidence-file <path>
scripts/verify_mem_service_release_certification.sh --ops-bundle-file <path> --remote-transport-bundle-file <path>
scripts/run_mem_service_release_certification_ci.sh
```

这部分设计的重点是：**生产认证不能靠文档声明，也不能靠本机 simulator smoke 冒充；必须由真实环境 evidence 进入 readiness gate。**

## 14. 验证现状

已经具备或已经使用过的验证入口：

```text
pytest guest-linux/aarch64/tests/test_guest_app_layout.py -q
pytest guest-linux/aarch64/tests/test_mem_service_daemon_runtime.py -q
make -C guest-linux/aarch64/apps/mem_service package-tarball-smoke
make -C guest-linux/aarch64/apps/mem_service package-deb-smoke
make -C guest-linux/aarch64/apps/mem_service installed-sdk-runtime-smoke
guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh --app llm_infer_mem_service
guest-linux/aarch64/scripts/run_ub_dual_node_apps.sh --app pretraining_client_mem_service
```

当前文档整理没有重新跑 QEMU 或 Linux ops CI。QEMU/guest 相关验证需要按项目约定在 sandbox 外执行，并在结束后检查没有残留 QEMU 进程。

## 15. 还差什么

### 15.1 生产 Linux ops 认证

还需要真实 Linux/root/systemd/rpm/promtool 环境跑：

```text
scripts/run_mem_service_linux_ops_ci.sh --rollback-rpm <previous-rpm> --rpm-file <current-rpm>
```

必须产出并复验：

```text
ops-certification-linux-ci.evidence
ops-certification-upgrade-rollback.marker
linqu-mem-service-ops-certification-bundle.tar
```

未完成前，不能宣称真实 systemd/rpm/Prometheus/Alertmanager/upgrade-rollback 生产认证完成。

### 15.2 跨主机 remote transport 认证

还需要真实 producer/consumer 分离环境，满足：

- source 不是 loopback
- producer host 和 consumer host 分离
- 有 network partition marker
- evidence 由 verifier 独立复验

目标入口：

```text
scripts/run_mem_service_remote_transport_ci.sh
scripts/verify_mem_service_remote_transport_bundle.sh
```

未完成前，remote payload backend 只能说有 loopback/TCP-loopback 机制验证和 evidence gate，不能说生产跨主机 transport 已认证。

### 15.3 安全模型增强

当前安全模型是 local-only/no-auth：

- Unix socket service endpoint
- loopback metrics endpoint

后续如果要面向真实多租户 serving/pretraining 集群，需要补：

- TLS/mTLS 或等价传输认证
- client identity
- ACL / namespace / tenant boundary
- audit 中的 caller identity
- secret/config 分发策略

### 15.4 分布式一致性和 HA

当前是单 daemon + durable local store 形态。生产多机服务还需要明确：

- 多 daemon 时的 ownership 模型
- record/catalog 复制策略
- leader election 或 sharding
- 跨节点恢复顺序
- split-brain 处理
- remote store 损坏后的恢复策略

### 15.5 数据面性能闭环

mem_service 已能管理 GVA/GSVA 相关 object/ref 元数据，但性能收益不由 mem_service 自动产生。

还需要分开验证：

- metadata RPC overhead
- prefix/KV hit 是否减少上游 decode 执行量
- payload copy 是否真的被 GVA/GSVA-backed path 替代
- OBMM/GSVA 数据面相对 legacy PA-to-UBA/sim-decoder path 的 microbenchmark
- end-to-end Qwen3 steps 中命中率、stall、copy、compute 的分项时间

### 15.6 SDK/API 产品化

当前 C SDK 可用，但还需要面向真实外部项目补：

- semantic versioning policy
- public header 最小化
- examples 按 serving/pretraining 场景拆得更清楚
- error handling guide
- upgrade guide
- API compatibility CI 覆盖发布前后包

## 16. 推荐后续计划

优先级按“离独立发布部署最近”排序：

1. 在真实 Linux CI 上跑通 `run_mem_service_linux_ops_ci.sh`，拿到 ops certification bundle。
2. 在真实两机或多机环境上跑通 `run_mem_service_remote_transport_ci.sh`，拿到 remote transport bundle。
3. 用两个 bundle 跑 `verify_mem_service_release_certification.sh`，让 `release-readiness` 从 not-certified 变成 certified。
4. 为 `llm_infer_mem_service` 和 `pretraining_client_mem_service` 建固定 CI gate，避免能力退化。
5. 做 GVA/GSVA 数据面分项 benchmark，把 metadata service 成本、payload transport 成本和 decode compute 节省拆开。
6. 安全模型从 local-only/no-auth 推进到可远端部署的认证授权模型。
7. 设计多 daemon / HA / remote catalog 的生产架构。

## 17. 一句话状态

`mem_service` 当前已经实现为可本地独立运行、可被 serving/pretraining 客户端消费、可打包并可自描述 release readiness 的 Memory Service；剩下的关键缺口不是再补 scaffold，而是在真实 Linux 和真实跨主机环境里跑出可复验的生产认证证据，并补齐远端安全与分布式可靠性模型。
