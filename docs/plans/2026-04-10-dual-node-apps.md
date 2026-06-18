# Dual-Node Apps Implementation Plan

Date: 2026-04-10

This historical plan has been superseded by the current standalone app layout.

The implemented apps are tracked by `guest-linux/aarch64/apps/README.md`.
The relevant current artifacts are:

| App | Source | Guest binary | Stable 2-node CLI |
| --- | --- | --- | --- |
| `ub_chat` | `guest-linux/aarch64/apps/ub_chat/ub_chat.c` | `/bin/linqu_ub_chat` | `guest-linux/aarch64/scripts/run_ub_dual_node_chat.sh` |
| `ub_rpc` | `guest-linux/aarch64/apps/ub_rpc/ub_rpc.c` | `/bin/linqu_ub_rpc` | `guest-linux/aarch64/scripts/run_ub_dual_node_rpc.sh` |
| `ub_udma` | `guest-linux/aarch64/apps/ub_udma/ub_udma.c` | `/bin/linqu_ub_udma` | `guest-linux/aarch64/scripts/run_ub_dual_node_udma.sh` |

Implementation rules:

- Keep app source in `guest-linux/aarch64/apps/<app>/`.
- Keep shared helpers in `guest-linux/aarch64/common/` or
  `guest-linux/aarch64/libs/`.
- Package apps through `guest-linux/aarch64/scripts/build_initramfs.sh`.
- Expose app validation through reusable scripts.
- Do not add hidden boot flows or compatibility wrappers under old runtime
  naming.
