# AArch64 Guest Apps

Each guest app owns one subdirectory under `apps/`.

Conventions:

- Source lives at `apps/<app_name>/<app_name>.c`, unless the installed binary has
  an established legacy name that must be preserved.
- Shared app-local helpers live next to the app source. Cross-app helpers belong
  in `guest-linux/aarch64/common/` or `guest-linux/aarch64/libs/`.
- `scripts/build_initramfs.sh` is the authoritative build and packaging entry for
  initramfs apps.
- `/bin/run_demo <action>` is the guest-side app launcher when an app needs an
  interactive or kernel-cmdline action.
- Multi-node validation belongs in a dedicated `scripts/run_ub_*_<app>.sh`
  runner when the app has observable 2-node, 4-node, or 8-node behavior.
