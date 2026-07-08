# W5 Manual Serving Run

This is the current manual entry for an 8-node W5 stream inference run. Use
`run_w5_cluster_config.sh` as the host-side entrypoint; lower-level runtime
scripts remain implementation details.

## Minimal Config

When running inside an openEuler container, install the native build
dependencies before the first run because the W5 entry may need to build the
workspace QEMU:

```sh
./guest-linux/aarch64/scripts/prepare_w5_container_deps.sh
```

The helper supports openEuler/Fedora/RHEL-like containers via `dnf`/`yum` and
Debian/Ubuntu containers via `apt-get`. It installs QEMU native build
dependencies and ensures the container's current `python3` can import
`distlib`.

For audit-only output:

```sh
./guest-linux/aarch64/scripts/prepare_w5_container_deps.sh --dry-run
```

Create a config file with runtime values only:

```sh
SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=2
SIM_QWEN3_SAMPLER_TOP_K=1
SIM_QWEN3_SAMPLER_TOP_P_MILLI=1000
SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI=1000
SIM_QWEN3_SAMPLER_SEED=0
SIM_W5_MEMORY_SERVICE=lingqu_memory_service
QEMU_MEM=8G
QEMU_SMP=2
SIM_W5_PROGRESS_INTERVAL_SECS=60
```

Validate it before launching:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --validate-only /path/to/w5.env
```

Run one-shot W5 stream inference:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh /path/to/w5.env
```

## Serving Queue

For sequential request submission, create a request file:

```text
request_id=req-001 prompt_token_ids=81378,37585,374 decode_steps=2
request_id=req-002 prompt_token_ids=151646,198,9707 decode_steps=2
```

Validate request syntax:

```sh
./guest-linux/aarch64/scripts/run_w5_serving_entry.sh --requests /path/to/requests.txt --validate-only
```

Start the 8-node cluster, submit the requests, and wait for completion:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh \
  --serve-requests /path/to/requests.txt \
  --nodea-ingress \
  /path/to/w5.env
```

`--nodea-ingress` means the host submits only to nodeA; nodeA publishes the
request to the other nodes through the guest-side serving control path.

## Current Boundary

Use `SIM_W5_TEST_*` only for validation gates, matrix runs, and evidence
collection. The config runner rejects the old non-`TEST` validation names such
as `SIM_W5_MEMORY_DECISION_STORE` and points to the replacement name.

The runtime store variables are still mainline variables and remain valid:

- `SIM_W5_MEMORY_SERVICE`
- `SIM_W5_MEMORY_STORE`
- `SIM_W5_MEMORY_OBJECT_STORE`
- `SIM_W5_MEMORY_ENGRAM_STATE`
- `SIM_W5_MEMORY_REGISTRY_DIR`
