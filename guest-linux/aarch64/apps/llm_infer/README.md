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
