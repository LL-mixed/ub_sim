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

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `llm_infer.c` into `/bin/linqu_llm_infer`.
- `apps/llm_infer/Makefile` and `apps/mem_service/Makefile` (in the standalone
  `mem_service` repository, via its `LLM_INFER_ROOT`) link the same
  component for app-local compile checks.
- `tests/test_guest_app_layout.py` validates that the `llm_infer` app consumes the
  component instead of owning these helpers directly.
