# W5 TEST Environment Variables

本文档记录 W5 中验证、测试、report、校验相关环境变量的命名边界。此类变量统一使用 `SIM_W5_TEST_` 前缀，避免继续和 W5 serving 主线运行语义混在一起。

## 命名规则

主线运行变量不带 `TEST`：

- `SIM_UAPI_W5_PROFILE`
- `SIM_W5_RUN_ID`
- `SIM_W5_SERVING_*`
- `SIM_W5_MEMORY_SERVICE`
- `SIM_W5_MEMORY_STORE`
- `SIM_W5_MEMORY_OBJECT_STORE`
- `SIM_W5_MEMORY_REGISTRY_DIR`

验证、测试、report、校验变量必须带 `TEST`：

- `SIM_W5_TEST_*`
- `SIM_W5_TEST_MEMORY_*`

外部使用者不应把 `SIM_W5_TEST_*` 当成 W5 serving 的稳定控制面。它们只服务于矩阵、回归、证据收集、health gate、prefix/shortpath/GSVA 功能验证。

`guest-linux/aarch64/scripts/run_w5_cluster_config.sh --print-env` 是当前
W5 入口的环境面审计命令。输出按以下分组：

- `runtime`: 主线运行和模型选择变量
- `serving`: serving request/queue/ingress 变量
- `test-memory-reuse`: Memory Service reuse、prefix/cache、GSVA、shortpath
  验证变量
- `test-maintenance`: post-run prune/health gate 变量
- `vendor-context-test`: fused-SIMT/vendor context 校验变量

## Runner / Health

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_VALIDATE_ONLY` | 只执行配置/路径校验 |
| `SIM_W5_TEST_REQUIRE_CONTEXT` | 要求指定上下文证据存在 |
| `SIM_W5_TEST_REQUIRE_PREFIX_CACHE` | 要求 prefix cache must-hit |
| `SIM_W5_TEST_ARTIFACT_KEEP_LATEST` | artifact prune 保留最近 N 个 run |
| `SIM_W5_TEST_POST_RUN_HEALTH` | run 后执行 health check |
| `SIM_W5_TEST_POST_RUN_PRUNE` | run 后执行 artifact prune |
| `SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES` | health gate 允许的可清理字节数 |
| `SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES` | health gate 允许的可清理候选数 |

## Artifact Size Guards

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MAX_MEMORY_STORE_JSON_BYTES` | Memory store JSON 最大字节数 |
| `SIM_W5_TEST_MAX_OBJECT_STORE_JSON_BYTES` | Object store JSON 最大字节数 |
| `SIM_W5_TEST_MAX_OBJECT_STORE_BIN_BYTES` | Object store bin 最大字节数 |
| `SIM_W5_TEST_MAX_SHORTPATH_STREAM_BYTES` | shortpath stream 最大字节数 |
| `SIM_W5_TEST_MAX_SHORTPATH_KV_STREAM_BYTES` | shortpath KV stream 最大字节数 |
| `SIM_W5_TEST_MAX_PREFIX_CACHE_KV_STREAM_BYTES` | prefix cache KV stream 最大字节数 |

## Memory Reuse / Decision Selectors

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MEMORY_DECISION_STORE` | 测试用 Memory Service decision store |
| `SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE` | decision store 对应 object store |
| `SIM_W5_TEST_MEMORY_OBSERVATION_STORE` | observation store |
| `SIM_W5_TEST_MEMORY_REUSE_DISABLE` | 禁用自动复用历史 store |
| `SIM_W5_TEST_MEMORY_REUSE_OUT_DIR` | 自动发现 reusable store 的目录 |
| `SIM_W5_TEST_MEMORY_REUSE_RUN_ID` | 旧 debug selector，当前应拒绝使用 |
| `SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG` | 显式指定 debug reuse run |
| `SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE` | 兼容用 post-run promote |
| `SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP` | runtime boundary lookup 开关 |
| `SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP` | online boundary lookup 开关 |
| `SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND` | boundary lookup backend |
| `SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_ID` | 单个 boundary observation selector |
| `SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_IDS` | 多个 boundary observation selector |
| `SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID` | 从指定 run 读取 boundary observations |
| `SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF` | boundary registry object ref |
| `SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT` | boundary registry entry count |

## Shortpath

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE` | shortpath lookup mode |
| `SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID` | 单个 shortpath decision selector |
| `SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_IDS` | 多个 shortpath decision selector |
| `SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID` | shortpath support evidence id |
| `SIM_W5_TEST_MEMORY_SHORTPATH_ACTION` | shortpath action，例如 `jump-to-terminal` |
| `SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE` | 是否执行 shortpath |
| `SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID` | shortpath artifact id |
| `SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND` | shortpath artifact kind |
| `SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM` | shortpath artifact checksum |
| `SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF` | shortpath artifact object ref |
| `SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START` | shortpath target layer start |
| `SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END` | shortpath target layer end |
| `SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START` | producer layer start |
| `SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END` | producer layer end |
| `SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION` | producer position |
| `SIM_W5_TEST_MEMORY_SHORTPATH_BOUNDARY_HIDDEN_BYTES` | boundary hidden payload bytes |
| `SIM_W5_TEST_MEMORY_SHORTPATH_BOUNDARY_HIDDEN_CHECKSUM` | boundary hidden checksum |
| `SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM` | shortpath proof checksum |
| `SIM_W5_TEST_MEMORY_SHORTPATH_STREAM` | inline shortpath stream |
| `SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT` | shortpath stream entry count |
| `SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH` | host shortpath stream file |
| `SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH_GUEST` | guest-staged shortpath stream file |
| `SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM` | inline shortpath KV stream |
| `SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT` | shortpath KV stream entry count |
| `SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH` | host shortpath KV stream file |
| `SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH_GUEST` | guest-staged shortpath KV stream file |

## Prefetch

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID` | prefetch plan id |
| `SIM_W5_TEST_MEMORY_PREFETCH_SCOPE` | prefetch scope |
| `SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX` | prefetch target step |
| `SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM` | prefetch plan checksum |
| `SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS` | prefetch artifact ids |
| `SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS` | prefetch artifact checksums |
| `SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS` | prefetch artifact refs |

## Prefix Cache

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP` | prefix cache lookup 开关 |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID` | prefix cache reuse plan id |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR` | 测试用 prefix cache service addr |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION` | prefix cache action |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID` | prefix cache artifact id |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM` | prefix cache artifact checksum |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF` | prefix cache artifact object ref |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS` | matched prefix token count |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM` | prefix cache proof checksum |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS` | suffix replay tokens |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM` | inline prefix-cache KV stream |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT` | prefix-cache KV stream count |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH` | host prefix-cache KV stream file |
| `SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH_GUEST` | guest-staged prefix-cache KV stream file |
| `SIM_W5_TEST_PREFIX_CACHE_SERVICE_PID` | 测试用 prefix cache service pid |

## GSVA / Approx Match / Output Check

| 新变量 | 作用 |
| --- | --- |
| `SIM_W5_TEST_MEMORY_GSVA_KV` | 测试 GSVA-backed KV path |
| `SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH` | GSVA stale/epoch guard |
| `SIM_W5_TEST_SHORTPATH_MATCH_MODE` | shortpath match mode |
| `SIM_W5_TEST_APPROXIMATE_REQUIRES_VERIFY` | approximate match 二次验证要求 |
| `SIM_W5_TEST_MIN_MATCH_SCORE_MILLI` | approximate match score threshold |
| `SIM_W5_TEST_MIN_SOURCE_CONFIDENCE_MILLI` | source confidence threshold |
| `SIM_W5_TEST_MIN_TERMINAL_MARGIN_MILLI` | terminal margin threshold |
| `SIM_W5_TEST_OUTPUT_TOKENIZER_DIR` | 输出检查使用的 tokenizer dir |
| `SIM_W5_TEST_EXPECT_OUTPUT_REGEX` | 期望输出正则 |
| `SIM_W5_TEST_REJECT_OUTPUT_REGEX` | 拒绝输出正则 |

## Deprecated Old Names

旧的非 `TEST` 名称不再作为 W5 测试/验证控制面使用。后续若发现新增验证变量没有 `SIM_W5_TEST_` 前缀，应先改名再接入脚本或测试。
