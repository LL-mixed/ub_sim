# Memory Service Component

`mem_service` owns the guest-side memory/object metadata service used by LLM
inference guest harnesses.

It is primarily a link-time component and also has a standalone smoke/inspect
CLI:

- `mem_service.c` implements the DB/object service and OBMM-backed runtime
  metadata paths.
- `mem_service_internal.h` contains the private include aggregate and service
  private compatibility shims shared by the split implementation units.
- `mem_service_compiler.h` contains local compiler annotations used by split
  implementation units.
- `mem_service_runtime_config.h` contains runtime wait defaults, environment
  parsing, and neutral run-id resolution.
- `mem_service_cluster_payload_contract.h` contains the device-independent
  cluster metadata payload wire format shared by guest and host service
  deployments.
- `mem_service_guest_runtime.h` contains guest OBMM cluster runtime state,
  mapped slots, queue descriptors, and region layout constants.
- `mem_service_object_contract.h` contains device-independent OBMM object
  kinds, fixed payload sizes, and layout constants that must stay reusable by
  guest and host service deployments.
- `mem_service_records.inc` contains the internal record-table allocation,
  lookup, and member helpers compiled into `mem_service.c`.
- `mem_service_qwen3_records.inc` contains Qwen3 streaming runtime record
  recycling policy; it must stay out of the generic record core.
- `mem_service_qwen3_record_policy.h` contains Qwen3 runtime record retention
  constants used by the model adapter record policy.
- `mem_service_qwen3_runtime.inc` contains Qwen3 runtime payload checksum, KV
  span allocation, engram object keys, and layer-range placement helpers.
- `mem_service_qwen3_placement.h` contains the Qwen3 layer-range placement
  contract used by the runtime range, KV, and object handoff flows.
- `mem_service_qwen3_runtime_range_wait_flow.inc` contains Qwen3 runtime range
  input wait, scheduler work-item resolution, and mapped payload view helpers.
- `mem_service_qwen3_runtime_range_publish_flow.inc` contains Qwen3 runtime
  range output, KV-state object publication, and downstream descriptor publish
  helpers.
- `mem_service_qwen3_kv_state_flow.inc` contains Qwen3 runtime range KV-state
  publish and previous-step resolve helpers.
- `mem_service_qwen3_terminal_token_flow.inc` contains Qwen3 terminal token
  publish, shortpath publish, and wait helpers.
- `mem_service_qwen3_engram_publish_flow.inc` contains Qwen3 engram candidate
  publish and decision-state publish helpers.
- `mem_service_qwen3_engram_wait_flow.inc` contains Qwen3 engram candidate,
  selected-token, history, and state wait helpers.
- `mem_service_qwen3_decode_barrier.inc` contains Qwen3 decode-round publish
  and all-node wait barrier helpers.
- `mem_service_keys.inc` contains device-independent key construction helpers
  that must stay reusable by guest and host service deployments.
- `mem_service_object_refs.inc` contains device-independent checksum and Lingqu
  OBMM object-ref projection helpers.
- `mem_service_obmm_objects.inc` contains OBMM object payload generation, kind
  naming, payload arena allocation, and object record publication helpers.
- `mem_service_metadata.inc` contains the prefix/KV metadata state machine used
  by both local metadata APIs and runtime-backed publication paths.
- `mem_service_cluster_payload.inc` contains the cluster metadata payload
  snapshot, compact summary, and local publish helpers.
- `mem_service_cluster_read.inc` contains stable cluster payload read, compact
  summary read, and slot record lookup helpers.
- `mem_service_cluster_utils.inc` contains cluster environment parsing, wait
  throttling, and OBMM region range update/sync helpers.
- `mem_service_cluster_runtime.inc` contains guest OBMM cluster bootstrap,
  export/import slot activation, and pool layout helpers.
- `mem_service_cluster_queue.inc` contains guest OBMM SPSC queue barriers,
  object descriptor publish/wait helpers, and pending descriptor matching.
- `mem_service_cluster_observe.inc` contains cluster metadata fetch, observe,
  and readiness summarization across local and remote payload snapshots.
- `mem_service_obmm_object_flow.inc` contains the guest OBMM object publish,
  descriptor exchange, remote resolve, and Qwen3 range handoff validation flow.
- `mem_service_qwen3.c` is the private adapter from mem_service placement/KV
  semantics to the model-neutral `llm_infer` Qwen3 topology helpers.
- `mem_service.h` exposes the service API consumed by guest apps.
- `mem_service_qwen3.h` exposes the Qwen3 runtime range/KV/engram adapter API
  to guest inference code that opts into that model path.
- `lingqu_object_service.h` defines the object-service payload contract.

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `mem_service.c` and `mem_service_qwen3.c`
  into the guest app binary.
- `apps/mem_service` builds `/bin/linqu_mem_service` for direct smoke and
  Qwen3 topology inspection.
- Guest app runners provide the CLI surface that exercises the component.
- `run_app mem_service` runs the standalone metadata smoke path.
- `tests/test_mem_service_record_recycling.py` validates record capacity, recycling,
  KV payload sizing, and object-ref naming contracts.

## Productization Split Contract

`mem_service` is being split toward a product-grade Lingqu data service that can
run as a guest component and as a host-side service for streaming LLM inference
and LLM pre-training data paths.

Keep the implementation layers separated:

- Core metadata: key construction, record tables, prefix/KV metadata state,
  object-reference projection, and validation. This layer must not depend on
  QEMU, OBMM device files, or Qwen3-specific topology.
- Transport/runtime: OBMM pool mapping, queue descriptors, cluster bootstrap,
  and guest handoff timing. This layer can depend on guest runtime facilities.
- Model adapters: Qwen3 range/KV/engram placement and payload sizing. New model
  families must be added as adapters rather than renaming or specializing the
  service core. Model-specific retention and recycling policies belong here.
- Deployment apps: guest CLI/app entrypoints and future host daemon entrypoints.
  They should consume the same core APIs and expose explicit command-line
  validation surfaces.

New code should move device-independent logic out of `mem_service.c` first,
then split transport and model adapters behind explicit headers. Do not add new
W4/W5-named public APIs to `mem_service`; W5 is a workload family, not the
service boundary.
