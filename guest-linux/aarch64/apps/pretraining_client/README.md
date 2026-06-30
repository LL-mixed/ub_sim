# Pretraining Client Guest App

`pretraining_client` builds `/bin/linqu_pretraining_client`, a guest-side
external client for validating that an independently running `linqu_mem_service`
daemon can serve pretraining workloads.

The app publishes and resolves durable pretraining records over the
`mem_service` socket API:

- dataset shard
- sample batch
- checkpoint
- gradient bucket
- optimizer state
- training step commit

The app-local `Makefile` builds the focused client binary. Initramfs packaging
still goes through `scripts/build_initramfs.sh`.
