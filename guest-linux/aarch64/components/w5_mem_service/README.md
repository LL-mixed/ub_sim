# W5 Memory Service Component

`w5_mem_service` owns the guest-side memory/object metadata service used by the
W4/W5 Qwen3 guest harness.

It is a link-time component, not a standalone demo app:

- `w4_kvcache_db_service.c` implements the DB/object service and OBMM-backed
  runtime metadata paths.
- `w4_kvcache_db_service.h` exposes the service API consumed by the W4/W5 guest
  app.
- `w4_lingqu_object_service.h` defines the object-service payload contract.
