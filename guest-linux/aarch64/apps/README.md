# AArch64 Guest Apps

Each guest app owns one subdirectory under `apps/`.

Conventions:

- Source lives at `apps/<app_name>/<app_name>.c`, unless the installed binary has
  an established legacy name that must be preserved.
- Shared app-local helpers live next to the app source. Cross-app helpers belong
  in `guest-linux/aarch64/common/` or `guest-linux/aarch64/libs/`.
- `scripts/build_initramfs.sh` is the authoritative build and packaging entry for
  initramfs apps.
- `/bin/run_app <action>` is the guest-side app launcher when an app needs an
  interactive action. Automated runners should prefer `rdinit=/bin/run_app`
  plus an app-specific `linqu_*` kernel-cmdline flag.
- Multi-node validation belongs in a dedicated `scripts/run_ub_*_<app>.sh`
  runner when the app has observable 2-node, 4-node, or 8-node behavior.

## App Validation Matrix

Each app must have a reusable CLI validation path. Use stable script entrypoints
instead of env-prefixed one-off shell commands.

| App | 2-node validation | Wider validation |
| --- | --- | --- |
| `ub_chat` | `scripts/run_ub_dual_node_apps.sh --app chat` | `scripts/run_ub_eight_node_chat_matrix.sh` |
| `ub_rpc` | `scripts/run_ub_dual_node_apps.sh --app rpc` | `scripts/run_ub_eight_node_rpc_matrix.sh` |
| `ub_tcp_each_server` | `scripts/run_ub_dual_node_apps.sh --app tcp_each_server` | `scripts/run_ub_eight_node_tcp_each_server_matrix.sh` |
| `ub_udma` | `scripts/run_ub_dual_node_apps.sh --app udma` | `scripts/run_ub_eight_node_udma_matrix.sh` |
| `ub_obmm_pool` | `scripts/run_ub_dual_node_apps.sh --app obmm_pool` | `scripts/run_ub_eight_node_obmm_pool.sh` |
| `obmm_queue` | `scripts/run_ub_dual_node_obmm_queue.sh` | `scripts/run_ub_eight_node_obmm_queue.sh` |
| `obmm_dataplane_microbench` | `scripts/run_ub_dual_node_obmm_dataplane_microbench.sh` | `scripts/run_ub_eight_node_obmm_dataplane_microbench.sh` |
| `obmm_import_stress` | `scripts/run_ub_dual_node_obmm_import_stress.sh` | `scripts/run_ub_eight_node_obmm_import_stress.sh` |
| `obmm_gsva` | `scripts/run_ub_dual_node_obmm_gsva.sh` | `scripts/run_ub_eight_node_obmm_gsva_matrix.sh` |
| `obmm_coh_test` | `scripts/run_ub_dual_node_obmm_coh_test.sh` | `scripts/run_ub_eight_node_obmm_coh_test.sh` |
| `gva_direct` | `scripts/run_ub_dual_node_gva_direct_test.sh` | `scripts/run_ub_dual_node_gva_direct_matrix.sh` |
| `gva_manager` | `scripts/run_ub_dual_node_gsva_manager_bootstrap.sh` | `scripts/run_ub_eight_node_gsva_manager_bootstrap.sh` |
| `gsva_query` | `scripts/run_ub_gsva_query_caps_test.sh` | `scripts/run_ub_eight_node_gsva_query_caps.sh` |
| `gsva_coh_test` | `scripts/run_ub_two_node_gsva_coh_test.sh` | `scripts/run_ub_eight_node_gsva_coh_test.sh` |
| `gsva_lifecycle_test` | `scripts/run_ub_two_node_gsva_lifecycle_test.sh` | `scripts/run_ub_eight_node_gsva_lifecycle_test.sh` |
| `npu_test` | `scripts/run_ub_two_node_npu_test.sh` | `scripts/run_ub_eight_node_npu_test.sh` |
| `npu_gsva_test` | `scripts/run_ub_two_node_npu_gsva_test.sh` | `scripts/run_ub_eight_node_npu_gsva_test.sh` |
| `ssd_test` | `scripts/run_ub_two_node_ssd_test.sh` | `scripts/run_ub_eight_node_ssd_test.sh` |
| `ssd_gsva_test` | `scripts/run_ub_two_node_ssd_gsva_test.sh` | `scripts/run_ub_eight_node_ssd_gsva_test.sh` |
| `w4_guest` | `scripts/run_ub_dual_node_w4_guest.sh` | `scripts/run_ub_eight_node_w4_guest.sh` |

`w5_mem_service` is not an app. It is a link-time component under
`components/w5_mem_service`; W5 validation uses
`scripts/run_w5_cluster_config.sh` as the stable entrypoint.
