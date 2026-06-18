# Dual-Node Apps Design

Date: 2026-04-10

This historical plan has been reconciled with the current app layout.

The originally planned chat, UDMA, and RPC workloads now live as standalone
guest apps:

| App | Source | 2-node validation | 8-node validation |
| --- | --- | --- | --- |
| `ub_chat` | `guest-linux/aarch64/apps/ub_chat/ub_chat.c` | `guest-linux/aarch64/scripts/run_ub_dual_node_chat.sh` | `guest-linux/aarch64/scripts/run_ub_eight_node_chat_matrix.sh` |
| `ub_udma` | `guest-linux/aarch64/apps/ub_udma/ub_udma.c` | `guest-linux/aarch64/scripts/run_ub_dual_node_udma.sh` | `guest-linux/aarch64/scripts/run_ub_eight_node_udma_matrix.sh` |
| `ub_rpc` | `guest-linux/aarch64/apps/ub_rpc/ub_rpc.c` | `guest-linux/aarch64/scripts/run_ub_dual_node_rpc.sh` | `guest-linux/aarch64/scripts/run_ub_eight_node_rpc_matrix.sh` |

Current conventions are maintained in
`guest-linux/aarch64/apps/README.md`. New app work should follow that matrix:
source under `apps/<app>/`, packaging through `scripts/build_initramfs.sh`, and
validation through stable script entrypoints rather than ad hoc environment
prefixes.
