# LLM Inference Component

`llm_infer` owns guest-side LLM inference helpers used by the inference harness.

It is a link-time component, not a standalone app. The current model option is
Qwen3; the component name intentionally stays model-neutral so additional model
families can be added without renaming the guest component.

Qwen3 ownership:

- Pipeline width, layer count, hidden byte sizing, model identifiers, and KV
  state byte sizing live here.
- `mem_service` consumes these helpers through `mem_service_qwen3.c`; it should
  not parse Qwen3 model/topology environment variables directly.

Managed model-result ownership:

- `llm_infer.h/.c` decode the existing Rust range-result serialization into
  allocation-relative hidden/KV subviews, without copying payloads or selecting
  a provider. The caller supplies the completed result's bounded CPU view and
  the expected dispatch coordinates, and retains its SDK CPU lease while
  decoding. Returned offsets alone grant no mapping or publication authority.
- Check the authoritative range-table pointer, entry checksum, coordinates,
  payload lengths and serialized KV checksum before returning either subview.
  Whole-result integrity remains the caller's provider/completion obligation;
  the probe checks its independently generated whole-result oracle first.
  Numerical layer/tensor checksums depend on the execution path. Keep those
  numerical metadata checksums separate from serialized payload checksums. Keep
  positional model checksums separate from byte-only Memory Service checksums.
- The existing `lingqu_shmem_pto_direct --managed-config` model probe consumes
  this decoder under its output owner's CPU lease. This is output decoding;
  V2 staging/sealing and the W5 serving-loop handoff remain separate operations.

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `llm_infer.c` into `/bin/linqu_llm_infer`.
- `apps/llm_infer/Makefile` and `apps/mem_service/Makefile` (in the standalone
  `mem_service` repository, via its `LLM_INFER_ROOT`) link the same
  component for app-local compile checks.
- `tests/test_guest_app_layout.py` validates that the `llm_infer` app consumes the
  component instead of owning these helpers directly.
