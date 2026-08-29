# Lingqu shmem PTO guest adaptor

This directory contains the guest-side, simulator-facing adaptor for PTO
dispatches whose tensor payload remains in `lingqu_shmem`.

- `lingqu_shmem_pto_guest.h` defines the bounded metadata input model.
- `lingqu_shmem_pto_guest.c` materializes the frozen tag-10 ABI v2 wire
  objects and computes their metadata CRC.
- `lingqu_shmem_pto_endpoint.h` exposes one opaque UBC CMDQ/CQ endpoint.
- `lingqu_shmem_pto_endpoint.c` owns queue pages, submits one tag-10 slot at a
  time, and decodes the existing completion prefix.

The metadata builder and endpoint never copy tensor payload bytes. They do not
expose QEMU, SIM_DEC, GVA, or GSVA identities through the public
`lingqu_shmem_memref` programming model. OBMM registration and public memref
lifetime ownership belong to the dedicated guest adaptor and workload layer.
