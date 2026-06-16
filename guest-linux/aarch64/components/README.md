# AArch64 Guest Components

Shared guest-side components live under `components/`.

Conventions:

- A component owns one subdirectory under `components/<component_name>/`.
- Public headers stay next to the component source unless they are shared across
  unrelated components, in which case they belong in `common/`.
- Components do not install guest binaries directly. Apps or harnesses link
  component sources and own the user-facing command name.
- `scripts/build_initramfs.sh` is the authoritative place that wires components
  into initramfs binaries.
