# OBMM Pool App Usage

`ub_obmm_pool` is a standalone guest app under
`guest-linux/aarch64/apps/ub_obmm_pool/`.

## Validation

Use the stable app runners:

```bash
zsh guest-linux/aarch64/scripts/run_ub_dual_node_obmm_pool.sh
zsh guest-linux/aarch64/scripts/run_ub_eight_node_obmm_pool.sh
```

## Runtime Knobs

| Variable | Default | Description |
| --- | --- | --- |
| `OBMM_POOL_EXPORT_SIZE_MB` | `512` | Shared memory exported by each node, in MiB. |
| `OBMM_IMPORT_CACHE_MODE` | `auto` | Import cache mode: `auto`, `nc`, or `cc`. |
| `OBMM_POOL_STRESS_ITERS` | `20` | Stress loop iterations in the dedicated runner. |

Examples:

```bash
OBMM_POOL_STRESS_ITERS=100 zsh guest-linux/aarch64/scripts/run_ub_dual_node_obmm_pool.sh
OBMM_IMPORT_CACHE_MODE=cc zsh guest-linux/aarch64/scripts/run_ub_dual_node_obmm_pool.sh
```

## W4/W5 Integration

W4/W5 guest flows link the shared memory/object service from
`guest-linux/aarch64/components/w5_mem_service/`. `ub_obmm_pool` remains an
independently validated app and is not used as a hidden legacy boot path.
