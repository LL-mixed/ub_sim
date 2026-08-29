# Lingqu shmem PTO guest adaptor

This directory contains the guest-side, simulator-facing adaptor for PTO
dispatches whose tensor payload remains in `lingqu_shmem`.

- `lingqu_shmem_pto_guest.h` defines the bounded metadata input model.
- `lingqu_shmem_pto_guest.c` materializes the frozen tag-10 ABI v2 wire
  objects and computes their metadata CRC.

The library handles metadata only. It never copies tensor payload bytes and it
does not expose QEMU, SIM_DEC, GVA, or GSVA identities through the public
`lingqu_shmem_memref` programming model. Endpoint MMIO submission belongs to
the dedicated guest workload layer.
