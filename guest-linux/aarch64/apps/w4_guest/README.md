# W4/W5 Guest App

`w4_guest` builds the `/bin/linqu_w4_guest` guest binary used by the W4/W5
Qwen3 harnesses.

The app owns the guest orchestration logic. Shared memory/object metadata lives
in `components/w5_mem_service/` and is linked into this app by
`scripts/build_initramfs.sh`.
