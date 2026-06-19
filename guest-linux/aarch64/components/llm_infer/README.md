# LLM Inference Component

`llm_infer` owns guest-side LLM inference helpers used by the W4/W5 guest
harness.

It is a link-time component, not a standalone app. The current model option is
Qwen3; the component name intentionally stays model-neutral so additional model
families can be added without renaming the guest component.

Build and validation entrypoints:

- `scripts/build_initramfs.sh` links `llm_infer.c` into `/bin/linqu_w4_guest`.
- `apps/w4_guest/Makefile` links the same component for app-local compile
  checks.
- `tests/test_guest_app_layout.py` validates that `w4_guest` consumes the
  component instead of owning these helpers directly.
