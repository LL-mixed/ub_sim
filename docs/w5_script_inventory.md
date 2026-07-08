# W5 Script Inventory

This document defines the current W5 script surface. Its purpose is to keep the
manual serving entry, internal runtime scripts, and validation scripts separate.

## Manual Entry

Use this script for manual W5 stream inference and sequential serving request
submission:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh /path/to/w5.env
```

For nodeA ingress serving requests:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh \
  --serve-requests /path/to/requests.txt \
  --nodea-ingress \
  /path/to/w5.env
```

## Internal Runtime

These scripts are implementation details behind the manual entry:

| Script | Role |
| --- | --- |
| `guest-linux/aarch64/scripts/run_w5_inference_cluster_runtime.sh` | W5-specific runtime orchestration and Memory Service bootstrap/reuse wiring |
| `guest-linux/aarch64/scripts/run_llm_infer_eight_node_guest.sh` | Generic 8-node llm_infer guest cluster runner used by W5 and legacy W4 wrappers |
| `guest-linux/aarch64/scripts/launch_ub_eight_node_headless.sh` | QEMU launch layer |
| `guest-linux/aarch64/scripts/w5_memory_reuse_common.sh` | Shared Memory Service reuse discovery for W5 runners and matrices |

Do not use `run_w5_inference_cluster_runtime.sh` as the manual entry unless
debugging the runtime layer directly.

## Serving Helpers

| Script | Role |
| --- | --- |
| `guest-linux/aarch64/scripts/run_w5_serving_entry.sh` | Shell wrapper for request-file parsing and validation |
| `guest-linux/aarch64/scripts/w5_serving_entry.py` | Request-file parser and metadata helper |
| `guest-linux/aarch64/scripts/run_w5_serving_submit.sh` | Shell wrapper for request submission to a running cluster |
| `guest-linux/aarch64/scripts/w5_serving_submit.py` | Host-side serving request submit/wait helper |

## Validation And Maintenance

These scripts are not W5 serving entries:

| Script | Role |
| --- | --- |
| `guest-linux/aarch64/scripts/run_w5_cluster_qwen3_0_6b_2step.sh` | Smoke shortcut that generates a qwen3 0.6B 2-step config and delegates to `run_w5_cluster_config.sh` |
| `guest-linux/aarch64/scripts/run_w5_prefix_cache_realistic_matrix.sh` | Prefix-cache functional matrix |
| `guest-linux/aarch64/scripts/run_w5_prefix_cache_serving_matrix.sh` | Prefix-cache serving-style validation matrix |
| `guest-linux/aarch64/scripts/w5_inference_run_report.py` | W5 summary/report parser |
| `guest-linux/aarch64/scripts/w5_cluster_health_check.py` | Artifact and process health gate |
| `guest-linux/aarch64/scripts/w5_artifact_prune.py` | W5 artifact listing/pruning helper |
| `guest-linux/aarch64/scripts/w5_inference_cluster_summary.py` | W5 alias for the generic guest summary parser |

## Compatibility Wrappers

| Script | Status |
| --- | --- |
| `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh` | Legacy W4 compatibility wrapper; it delegates to `run_llm_infer_eight_node_guest.sh` |

Removed W5 compatibility wrappers:

| Removed script | Replacement |
| --- | --- |
| `guest-linux/aarch64/scripts/run_ub_w5_inference_cluster.sh` | `run_w5_cluster_config.sh` for users, `run_w5_inference_cluster_runtime.sh` internally |
| `guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh` | `run_w5_inference_cluster_runtime.sh` |
