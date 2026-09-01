# Lingqu shmem PTO guest adaptor

This directory contains the guest-side, simulator-facing adaptor for PTO
dispatches whose tensor payload remains in `lingqu_shmem`.

- `lingqu_shmem_pto_guest.h` defines the bounded metadata input model.
- `lingqu_shmem_pto_guest.c` materializes the frozen tag-10 ABI v2 wire
  objects and computes their metadata CRC.
- `lingqu_shmem_pto_endpoint.h` exposes one opaque UBC CMDQ/CQ endpoint.
- `lingqu_shmem_pto_endpoint.c` owns queue pages, submits one tag-10 slot at a
  time, and decodes the existing completion prefix.
- `lingqu_shmem.h` is the public opaque region/memref programming model.
- `lingqu_shmem_sim.h` is the simulator-only region attachment seam.
- `lingqu_shmem_pto.h` converts public memrefs into one in-flight dispatch and
  keeps each region alive until completion.

The metadata builder and endpoint never copy tensor payload bytes. They do not
expose QEMU, SIM_DEC, GVA, or GSVA identities through the public
`lingqu_shmem_memref` programming model. OBMM registration and public memref
lifetime ownership belong to the dedicated guest adaptor and workload layer.
The current lifetime API is single-threaded; concurrent create/destroy and
multiple in-flight dispatches remain part of the P4 concurrency gate. The
caller must keep the underlying mapping and OBMM registration active until
all in-flight objects have been finished and every memref has been destroyed.

The P3 two-node acceptance workload contract is documented under
`apps/lingqu_shmem_pto_direct/`. Its implementation consumes the public memref
API and keeps simulator identities inside `lingqu_shmem_sim_region_desc`.

## Memory Service compute adapter

`lingqu_shmem_mem_service.h` is the model-facing bridge from a resolved Memory
Service object view to a dispatch-bound `lingqu_shmem_memref`. Callers provide
only the object view and tensor geometry. They do not select a transport or
interpret an OBMM mapping identifier.

The implementation is split into two translation units:

- `lingqu_shmem_mem_service.c` owns provider-neutral validation, memref
  construction, lease accounting, and release ordering;
- `lingqu_shmem_mem_service_obmm.c` owns the QEMU guest OBMM mapping
  registration used by the current eight-node deployment.

Every acquired memref has one lease. The lease pins the provider mapping until
the caller has observed PTO completion and calls
`lingqu_shmem_mem_service_release()`. Context close fails with `-EBUSY` while a
lease is active. Model code keeps passing Memory Service ObjectRefs for
identity, placement, version, and commit semantics; the compute adapter only
materializes the temporary data-plane view.

Local output follows a two-address rule. `lingqu_shmem_memref` carries the
self-import alias address registered for Simpler/PTO `TSTORE`. The returned
`lingqu_shmem_mem_service_local_buffer.data` points at the original local
Memory Service OBMM payload arena. After dispatch completion, the caller may
release the alias lease and publish the original arena range in place with its
`backing_offset`; tensor payload bytes stay in the same OBMM allocation.

The current in-place publish contract is limited to the trusted in-process
model runtime. Memory Service checks arena bounds, length, and exact pointer
identity. A separately minted allocation token and concurrent allocation owner
check are deferred to the concurrency and recovery gate.
