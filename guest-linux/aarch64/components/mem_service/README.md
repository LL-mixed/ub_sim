# Memory Service Component

`mem_service` owns the guest-side memory/object metadata service used by LLM
inference guest harnesses.

It is a link-time component, not a standalone app:

- `mem_service.c` implements the DB/object service and OBMM-backed
  runtime metadata paths.
- `mem_service.h` exposes the service API consumed by guest apps.
- `lingqu_object_service.h` defines the object-service payload contract.

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `mem_service.c` into the guest app binary.
- Guest app runners provide the CLI surface that exercises the component.
- `tests/test_mem_service_record_recycling.py` validates record capacity, recycling,
  KV payload sizing, and object-ref naming contracts.
