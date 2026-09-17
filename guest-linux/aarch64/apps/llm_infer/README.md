# LLM Inference Guest App

`llm_infer` builds the `/bin/linqu_llm_infer` guest binary used by the Qwen3
inference harnesses.

The app owns the guest orchestration logic. Shared memory/object metadata lives
in `components/mem_service/` in the root-level `mem_service/` submodule
(referenced through `MEM_SERVICE_ROOT`) and is linked into this app by
`scripts/build_initramfs.sh`. Shared LLM inference helpers live in
`components/llm_infer/`.

The app-local `Makefile` builds the same `linqu_llm_infer` binary for focused
compile checks. Initramfs packaging still goes through `scripts/build_initramfs.sh`.

Managed W5 serving defaults the OBMM allocation granularity to 64 MiB and
passes the same value to the built-in or loadable OBMM allocator through
`obmm.mem_allocator_granu`. A callable-4 output view is smaller than one
allocator granule, so strict PTO registration observes one physically
contiguous backing range. The managed runtime also sizes its OBMM readiness
canary to that granule instead of assuming a fixed 2 MiB region.
`SIM_W5_MANAGED_IMPORT_REGION_BYTES` must be at least the configured
`SIM_W5_MANAGED_ALLOCATION_GRANULARITY_BYTES`; the runner and guest reject
smaller or non-power-of-two configurations before opening the provider.
