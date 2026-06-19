# Memory Service Component

`mem_service` owns the guest-side memory/object metadata service used by the
W4/W5 Qwen3 guest harness.

It is a link-time component, not a standalone app:

- `w4_kvcache_db_service.c` implements the DB/object service and OBMM-backed
  runtime metadata paths.
- `w4_kvcache_db_service.h` exposes the service API consumed by the W4/W5 guest
  app.
- `w4_lingqu_object_service.h` defines the object-service payload contract.

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `w4_kvcache_db_service.c` into
  `/bin/linqu_w4_guest`.
- `scripts/run_ub_four_node_w4_guest.sh`,
  `scripts/run_ub_eight_node_w4_guest.sh`, and the W5 inference runners provide
  the CLI surface that exercises the component.
- `tests/test_w4_db_record_recycling.py` validates record capacity, recycling,
  KV payload sizing, and object-ref naming contracts.
