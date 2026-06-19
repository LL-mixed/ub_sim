# Memory Service Component

`mem_service` owns the guest-side memory/object metadata service used by LLM
inference guest harnesses.

It is primarily a link-time component and also has a standalone smoke/inspect
CLI:

- `mem_service.c` implements the DB/object service and OBMM-backed
  runtime metadata paths.
- `mem_service_qwen3.c` is the private adapter from mem_service placement/KV
  semantics to the model-neutral `llm_infer` Qwen3 topology helpers.
- `mem_service.h` exposes the service API consumed by guest apps.
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
