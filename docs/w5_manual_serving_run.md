# W5 Manual Serving Run

This is the current manual entry for a W5 stream inference run. Use
`run_w5_cluster_config.sh` as the host-side entrypoint; lower-level runtime
scripts remain implementation details.

The same entry supports 2-node, 3-node, and 8-node clusters. Select the active
topology with `--nodes 2`, `--nodes 3`, or `--nodes 8`; the default is 8. The
entry derives the QEMU fabric topology, simulator scenario, port count, active
guest list, layer partition, handoff chain, and validation counts from that
single value. Do not put those derived values in the env file.

## Minimal Config

On a Docker test bed such as `hw-910c`, use the host-side container entry. It
starts Docker, prepares container dependencies, builds QEMU when needed, and
then delegates to `run_w5_cluster_config.sh`:

```sh
./guest-linux/aarch64/scripts/run_w5_in_container.sh w5.env
```

For a smaller pipeline topology:

```sh
./guest-linux/aarch64/scripts/run_w5_in_container.sh \
  -- --nodes 3 w5.deepseek-v4-flash-simpler.env
```

For serving requests:

```sh
./guest-linux/aarch64/scripts/run_w5_in_container.sh \
  -- --serve-requests requests.txt --nodea-ingress w5.env
```

The internal container dependency helper supports openEuler/Fedora/RHEL-like
containers via `dnf`/`yum` and Debian/Ubuntu containers via `apt-get`. It
installs QEMU native build dependencies and ensures the container's current
`python3` can import `distlib`.

For audit-only output of the host-side Docker command:

```sh
./guest-linux/aarch64/scripts/run_w5_in_container.sh --dry-run w5.env
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

Prepare only the W5 Memory Service runtime surface:

```sh
./guest-linux/aarch64/scripts/run_w5_memory_service_bootstrap.sh \
  --print-env \
  --env-file /tmp/w5-memory-service.env
```

`run_w5_cluster_config.sh` calls this bootstrap entry automatically when the W5
memory path is enabled. The lower-level
`run_w5_inference_cluster_runtime.sh` does not bootstrap infrastructure; it
expects `SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED=1` and fails fast otherwise. The
bootstrap entry is deliberately separate from infer execution and is owned by
the `mem_service` host binary. It prepares the service runtime surface and emits
sourceable env. W5 infer and serving queue consume that env as clients; request
data, prefix/KV records, and shortpath decisions must be written through
Memory Service APIs or clients rather than by the infer bootstrap path.

Run one-shot W5 stream inference:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh /path/to/w5.env
```

Run the same model profile on two or three simulated nodes:

```sh
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --nodes 2 /path/to/w5.env
./guest-linux/aarch64/scripts/run_w5_cluster_config.sh --nodes 3 /path/to/w5.env
```

Use `run_w5_cluster_config.sh` directly only when already inside a prepared
container or on a host with QEMU build dependencies installed.

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

Start the cluster, submit the requests, and wait for completion:

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
- `SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED`
- `SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE`
- `SIM_W5_MEMORY_STORE`
- `SIM_W5_MEMORY_OBJECT_STORE`
- `SIM_W5_MEMORY_ENGRAM_STATE`
- `SIM_W5_MEMORY_REGISTRY_DIR`
