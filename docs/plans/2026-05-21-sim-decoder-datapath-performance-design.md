# SIM Decoder Imported PA Data-Path Performance Design

## 1. Goal

This document records the current imported-PA data path investigation and defines the performance-oriented redesign plan for QEMU SIM_DEC cross-node memory access.

The target path is:

```text
guest obmm_import
  -> sim decoder MAP
  -> guest CPU load/store on imported PA
  -> QEMU SIM_DEC data path
  -> UB link transport
  -> home-side SIM_UMMU token/UBA memory access
```

The current implementation proves the correctness model, but it is not a performance path. The next phase should keep the current correctness contract while replacing the most expensive per-access mechanisms with batched, cached, and asynchronous mechanisms.

Terminology note: SIM_DEC is the importer/user-side simulated decoder service. It maps local imported PA ranges to remote UBMD-like `{remote_uba, token_id, dcna}` targets. The home side does not perform decoder mapping for that imported PA. Its role is SIM_UMMU: execute token/UBA based UMMU memory access against home memory for UB data-path requests.

SIM_UMMU is not private to SIM_DEC. SIM_DEC READ/WRITE messages, UDMA/URMA READ/WRITE packets, and future UB data-path protocols should all converge on the same home-side SIM_UMMU execution boundary once they reach the home UBC.

## 2. Questions & Answers

### 2.1 Does guest imported-PA access start as normal load/store?

Yes. After `obmm_import`, the import region owns a local PA range. User mapping is created by `map_import_region()` through `remap_pfn_range()`, so application access is ordinary guest CPU load/store against the imported PA range.

Relevant code:

1. `guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c`
   - `obmm_import()`
   - `prepare_import_memory()`
   - `obmm_sim_dec_map_import()`
   - `map_import_region()`
2. `guest-linux/kernel_ub/drivers/ub/obmm/obmm_sim_decoder.h`
   - `obmm_sim_dec_import_priv_v1`
   - import callback contract

### 2.2 Does that load/store reach SIM_DEC?

Yes, but not through `sim_dec_lookup_by_pa()` first.

The CPU load/store path is intercepted by QEMU because SIM_DEC MAP installs an overlapping MemoryRegion at the imported PA:

```c
memory_region_init_io(&entry->cpu_window, ..., &sim_dec_cpu_window_ops, ...);
memory_region_add_subregion_overlap(get_system_memory(), entry->local_pa,
                                    &entry->cpu_window, 10);
```

Therefore the primary imported-PA CPU path is:

```text
guest CPU load/store imported PA
  -> QEMU memory dispatch
  -> sim_dec_cpu_window_read/write
  -> ubc_sim_dec_remote_read/write
```

`sim_dec_lookup_by_pa()` is still useful, but it applies to UBC DMA data-path accesses that hit a decoder-mapped PA, not to the normal CPU MemoryRegion interception path.

### 2.3 How does SIM_DEC send WRITE/READ to the home node?

SIM_DEC currently uses UB link messages over Unix domain sockets.

Current path:

```text
sim_dec_cpu_window_write/read
  -> ubc_sim_dec_remote_write/read
  -> ubc_send_msg_over_link
  -> ub_link_write_message
  -> QIOChannelSocket
```

Important details:

1. `ubc_send_msg_over_link()` wraps the payload in `MsgPktHeader`.
2. The packet uses `hdr->ulh.cfg = UB_CLAN_LINK_CFG`.
3. The packet uses `msg_code = UBC_MSG_CODE_URMA_DATA`.
4. SIM_DEC operation is identified by `sub_msg_code`:
   - `UBC_MSG_SUB_SIM_DEC_WRITE`
   - `UBC_MSG_SUB_SIM_DEC_READ_REQ`
   - `UBC_MSG_SUB_SIM_DEC_READ_RESP`
5. `ub_link_write_message()` writes a 4-byte frame length followed by the packet bytes to `QIOChannelSocket`.
6. `ub_link_kick_remote()` also creates a kick file for the peer endpoint.

Relevant code:

1. `vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c`
   - `sim_dec_cpu_window_read()`
   - `sim_dec_cpu_window_write()`
   - `ubc_send_msg_over_link()`
   - `ubc_sim_dec_remote_write()`
   - `ubc_sim_dec_remote_read()`
2. `vendor/qemu_8.2.0_ub/hw/ub/ub_link.c`
   - `ub_link_setup_socket()`
   - `ub_link_write_message()`
   - `ub_link_read_message()`
   - `ub_link_kick_remote()`
3. `vendor/qemu_8.2.0_ub/include/hw/ub/ub_link.h`
   - `QIOChannel *ioc`
   - `QIONetListener *lioc`
   - `socket_path`

### 2.4 Is current UB link transport efficient enough?

No. It is adequate for functional validation, not for high-frequency data-plane traffic.

Current performance costs:

1. CPU MemoryRegion access is limited to 1/2/4/8-byte operations.
2. Each imported-PA CPU read can become a synchronous cross-node request/response.
3. Each imported-PA CPU write can become a small socket packet.
4. `ub_link_write_message()` copies packet bytes into a framed socket stream.
5. `ub_link_read_message()` copies from socket into `rx_buf`, then copies complete frames again.
6. `ub_link_kick_remote()` uses file creation as an extra wakeup path.
7. The read path waits in a loop with `ub_fm_poll_rx_links_now()`, `ubc_sim_dec_process_wait_links()`, and `g_usleep(1000)`.

This makes the current design a correctness bridge, not a realistic imported-memory data-plane backend.

### 2.5 Does home-side SIM_UMMU have a simulated TLB?

Yes. The implementation has a small UMMU IOTLB backed by `GHashTable`.

Current path:

```text
home UBC receives SIM_DEC or UDMA/URMA data-path READ/WRITE
  -> SIM_UMMU execution boundary
  -> ubc_handle_sim_dec_rx_read_req/write
     or ubc_handle_urma_rx_write/read_request
  -> ubc_dma_read/write_local_data_tid_strict
  -> address_space_read/write(UMMU AddressSpace)
  -> ummu_translate
  -> ummu_iotlb_lookup
  -> page-table walk on miss
  -> ummu_iotlb_insert
```

Relevant code:

1. `vendor/qemu_8.2.0_ub/hw/ub/ub_ummu.c`
   - `ummu_translate()`
   - `ummu_iotlb_lookup()`
   - `ummu_iotlb_insert()`
   - `ummu_iotlb_inv_all()`
   - `ummu_iotlb_inv_tecte_tag()`
2. `vendor/qemu_8.2.0_ub/include/hw/ub/ub_ummu.h`
   - `UMMU_IOTLB_MAX_SIZE 256`

### 2.6 Is current UMMU IOTLB efficient enough?

It is useful, but still not enough for a performance data path.

Current limitations:

1. Capacity is fixed at 256 entries.
2. Replacement removes an arbitrary first hash-table entry, not an LRU or clock victim.
3. Lookup probes all VMSA levels.
4. Miss path performs page-table walk by reading guest memory PTEs.
5. It is not coordinated with SIM_DEC imported-PA locality or UB link batching.

The IOTLB reduces repeated PTW cost, but the dominant cost remains the imported-PA CPU window and socket-per-small-access behavior.

## 3. Current End-to-End Behavior

### 3.1 Import setup

```text
obmm_import()
  -> init_import_region_from_cmd()
  -> prepare_import_memory()
      -> occupy_addr_range()
      -> setup_pa()
      -> setup_iomem_resource()
  -> obmm_sim_dec_map_import()
      -> parse remote_uba/token_value from region->priv
      -> invoke registered sim decoder import callback
      -> QEMU SIM_DEC_OP_MAP
  -> register_obmm_region()
  -> activate_obmm_region()
```

### 3.2 SIM_DEC MAP in QEMU

```text
ubc_handle_sim_dec_message()
  -> sim_dec_handle_map()
      -> validate size/token
      -> reject overlap
      -> allocate SimDecMapEntry
      -> record local_pa, size, remote_uba, token_id, dcna
      -> install cpu_window at local_pa
```

### 3.3 Imported-PA CPU write

```text
guest store imported_va
  -> guest PA = imported local_pa + offset
  -> QEMU cpu_window write
  -> sim_dec_cpu_window_write()
      -> remote_uba = entry->remote_uba + offset
      -> ubc_sim_dec_remote_write()
          -> chunk to UBC_SIM_DEC_WRITE_CHUNK_MAX
          -> send UBC_MSG_SUB_SIM_DEC_WRITE over UB link
  -> home ubc_msgq receives packet
  -> ubc_handle_sim_dec_rx_write()
      -> ubc_dma_write_local_data_tid_strict()
      -> UMMU address_space_write()
```

### 3.4 Imported-PA CPU read

```text
guest load imported_va
  -> guest PA = imported local_pa + offset
  -> QEMU cpu_window read
  -> sim_dec_cpu_window_read()
      -> remote_uba = entry->remote_uba + offset
      -> optional sync_shadow hit
      -> ubc_sim_dec_remote_read()
          -> send UBC_MSG_SUB_SIM_DEC_READ_REQ
          -> wait for UBC_MSG_SUB_SIM_DEC_READ_RESP
  -> home ubc_msgq receives read request
  -> ubc_handle_sim_dec_rx_read_req()
      -> ubc_dma_read_local_data_tid_strict()
      -> UMMU address_space_read()
      -> send UBC_MSG_SUB_SIM_DEC_READ_RESP
```

## 4. Performance Problem Statement

The current path pays a cross-node message and QEMU dispatch cost at the wrong granularity.

The correctness model treats imported PA as synchronous remote memory. The implementation, however, maps each small CPU load/store into a small synchronous or semi-synchronous socket message. This creates excessive overhead from:

1. QEMU MemoryRegion callback per scalar access.
2. Packet allocation and header construction per small access.
3. Unix socket write/read framing and copies.
4. File-based kick side effects.
5. Synchronous read wait loop.
6. Remote UMMU translation per small access, even when locality is page/block oriented.

The first-principles target should be:

```text
Preserve imported-memory semantics at the page/range level,
but avoid remote traffic and UMMU translation at scalar access granularity.
```

## 5. Target Architecture

```text
guest imported PA load/store
  -> QEMU SIM_DEC imported window
      -> local hot-page cache / shadow window
      -> batched dirty tracking
      -> async writeback queue
      -> read prefetch / miss fill
  -> UB link data transport
      -> shared-memory ring or batched socket fallback
  -> home SIM_UMMU translation cache
      -> expanded IOTLB
      -> range/page translation cache
      -> token-aware invalidation
```

## 6. Phased Plan

### Phase 0: Instrument the current path

Goal: make performance bottlenecks visible before changing behavior.

Tasks:

1. Add counters for `sim_dec_cpu_window_read/write`:
   - access size histogram
   - map id
   - offset page
   - read/write counts
   - remote read/write bytes
   - shadow hits/misses
2. Add UB link counters:
   - packets by `sub_msg_code`
   - bytes by `sub_msg_code`
   - write retries
   - bounded write timeout count
   - kick file count
3. Add UMMU counters:
   - IOTLB lookup count
   - IOTLB hit/miss
   - PTW count
   - invalidation count
   - translation failure count
4. Expose summary logs at QEMU shutdown and optionally every N seconds.
5. Add one dedicated stress mode that only exercises OBMM imported PA read/write, without real inference coupling.

Acceptance:

1. A single-node or dual-node stress run reports per-stage counters.
2. Counters distinguish CPU-window path from UBC DMA `sim_dec_lookup_by_pa()` path.
3. Baseline numbers can be compared before/after each later phase.

### Phase 1: Imported-window page cache and read prefetch

Goal: stop sending remote read messages for every small CPU load.

This cache is host-side QEMU state behind the SIM_DEC imported-PA MemoryRegion. It is transparent to the guest and must not require changing how the guest maps the imported PA range. In particular, this phase does not change whether the guest VMA is cacheable or non-cacheable. It only changes how QEMU satisfies imported-window reads after the guest access has already trapped into the SIM_DEC CPU window.

Design:

1. Maintain per-map page cache entries:
   - `map_id`
   - `page_index`
   - `remote_uba_base`
   - `valid`
   - `dirty`
   - `last_used`
   - fixed page buffer
2. On CPU read:
   - if page cache hit, return bytes locally.
   - if miss, fetch one page or configurable chunk from home using SIM_DEC read.
3. On sequential access:
   - prefetch next page or range.
4. Keep existing `sync_shadow` only as a compatibility mechanism, or fold it into the page cache.

Constraints:

1. Read cache must be invalidated on unmap.
2. If write-through mode remains enabled, writes update the local cache after remote write succeeds.
3. Cache size must be bounded per map and globally.
4. Read cache is valid only under a visibility/ownership contract that excludes concurrent home-side or peer writes to the same bytes, or it must be invalidated/refilled on explicit sync, barrier, or ownership transition.

Acceptance:

1. Repeated scalar reads from the same imported page produce one remote read, then local hits.
2. Random reads across M pages produce roughly M remote reads, not one per scalar access.
3. Existing OBMM import/unimport tests still pass.

### Phase 2: Write coalescing and async writeback

Goal: stop sending remote write messages for every small CPU store.

This phase must not weaken the guest-visible memory model. For a guest mapping that is non-cacheable/device-like, a completed store is expected to be externally visible at the home side before the guest can rely on subsequent ordering. A hidden host-side write-back cache would violate that expectation unless the mapping is explicitly configured to a relaxed/write-back mode.

Therefore the safe default remains write-through. Async writeback is an opt-in optimization mode with an explicit visibility contract.

Async writeback also needs an explicit guest-to-SIM_DEC barrier/sync interface. Without that interface, QEMU cannot know when buffered writes must become visible at the home side, so the optimization would be unusable for any mapping that needs deterministic visibility.

Design:

1. CPU write updates local page cache immediately.
2. Mark dirty byte ranges inside the page.
3. Flush dirty ranges by:
   - explicit OBMM flush/sync
   - unmap/release
   - capacity eviction
   - optional periodic flush
4. Coalesce adjacent dirty ranges before sending.
5. Preserve a strict mode for tests that require immediate remote visibility.
6. Add a SIM_DEC writeback barrier/sync operation that guest OBMM can call.

Correctness modes:

1. `write-through`: current semantics; every write goes remote.
2. `write-back`: local dirty cache, flush on sync/unmap.
3. `write-combine`: coalesce within a short time window, then remote write.
4. `buffered-with-explicit-sync`: guest stores may complete before home-side visibility, but `SIM_DEC_SYNC/BARRIER` completion guarantees all covered dirty bytes are visible at the home side.

Default should stay conservative until tests cover visibility semantics.

Memory-model requirements:

1. `write-through` is the default for non-cacheable/imported-device mappings. A CPU store returns only after the remote SIM_DEC write has completed at the home-side SIM_UMMU execution boundary.
2. `write-combine` may delay transmission only when the guest mapping or import contract explicitly opts into relaxed visibility. It must flush on barriers, sync, unmap, ownership transfer, and any operation that advertises remote visibility.
3. `write-back` requires an explicit opt-in contract and must be reported in diagnostics. It is not transparent for non-cacheable mappings.
4. If the implementation cannot observe guest barriers for a given mapping type, that mapping type must stay `write-through`.
5. Failed async writeback must poison the map or surface a deterministic error state; silent data loss is not acceptable.
6. Non-cacheable guest mappings may use host-side write buffering only under an explicit `buffered-with-explicit-sync` import contract. In that mode, non-cacheable still describes guest CPU caching behavior, while SIM_DEC defines remote visibility at explicit sync/barrier points.

Required guest-facing interfaces:

1. `ub_sim_decoder_sync(map_id, offset, len)` must mean: flush all buffered writes in the covered range to the home side and wait until SIM_UMMU has executed them and returned a home-side acknowledgement. After success, the covered writes are visible to home-side observers under the declared import contract.
2. Add a stronger named helper if needed, for example `ub_sim_decoder_write_barrier(map_id, offset, len, flags)`, to avoid overloading existing cache-maintenance language.
3. OBMM must call the barrier/sync helper from:
   - `flush_import_region()` for writeback-capable cache ops
   - `ub_mem_drain_start/state` or equivalent ownership-transfer path
   - import-region unmap/release
   - any future ioctl/API that promises remote visibility
4. QEMU SIM_DEC should treat `SIM_DEC_OP_SYNC` as the initial backend operation for this contract, or split a new `SIM_DEC_OP_BARRIER` if read-cache invalidation and writeback ordering need different semantics.
5. The sync response must carry success/failure status. On failure, OBMM must not report the flush/drain/unmap operation as cleanly completed.

Acceptance:

1. Small repeated stores into one page coalesce into fewer remote writes.
2. `flush_import_region()` or `ub_sim_decoder_sync()` pushes dirty data to home.
3. Unmap flushes dirty data before removing mapping.
4. Fault injection can force writeback failure and verify rollback/error reporting.
5. A buffered non-cacheable import proves that home-side memory is stale before sync and up to date after sync.

### Phase 3: UB link batched transport

Goal: keep socket fallback, but add a true data-plane transport.

This phase must be visible to SIM_DEC and SIM_UMMU at the semantic boundary. It is not enough to hide batching inside `ub_link_write_message()`: SIM_DEC barrier/sync/drain needs to know when all writes covered by a barrier have been transmitted, executed by home-side SIM_UMMU, and acknowledged. Otherwise a barrier could complete after data only reached a local batch queue, which would violate the explicit sync contract from Phase 2.

Design options:

1. Shared-memory ring plus eventfd:
   - one ring per link direction.
   - descriptors point to payload slots.
   - eventfd wakes peer QEMU.
2. Batched socket fallback:
   - aggregate multiple SIM_DEC operations into one frame.
   - remove per-message kick file when socket AIO is active.
   - keep kick file only as recovery/fallback.

Preferred staged implementation:

1. First add SIM_DEC batch message format over current socket.
2. Then add shared-memory ring transport behind the same `ub_link_write_message` abstraction.
3. Keep current socket path as compatibility fallback.

Required transport-visible contract:

1. SIM_DEC write operations must carry an ordered sequence number or barrier epoch.
2. The transport must provide a flush API, for example `ub_link_flush_until(seqno)` or `ub_link_submit_barrier(epoch)`, that means all prior submitted bytes have left the local batch/ring.
3. SIM_DEC sync/barrier must require a home-side SIM_UMMU execution acknowledgement, not only local transmission completion.
4. Batched writes need per-op or per-epoch status so a failed op can poison the map and fail the guest sync.
5. Read requests are already request/response; write batching must add equivalent completion semantics for barrier-covered writes.
6. The socket fallback and shared-memory ring path must expose the same completion semantics to SIM_DEC.

Batch payload format:

```text
SimDecBatchHdr {
    version
    op_count
    flags
}
SimDecBatchOp[op_count] {
    op
    seqno
    barrier_epoch
    map_id or token_id
    remote_uba
    len
    payload_offset
}
payload bytes
```

Acceptance:

1. A batch can carry multiple writes and multiple read requests.
2. Per-op status is returned for read/error cases.
3. Existing single-message protocol remains supported.
4. Benchmarks show reduced packet count and wakeup count.
5. `SIM_DEC_SYNC/BARRIER` waits for home-side acknowledgement of all prior writes in the covered epoch.
6. A fault-injected partial batch failure prevents barrier completion and surfaces a deterministic guest-visible error.

### Phase 4: SIM_UMMU IOTLB improvement

Goal: make home-side SIM_UMMU translation cost proportional to page/range locality, not scalar accesses. This must benefit both SIM_DEC imported-PA traffic and UDMA/URMA data-path traffic, since both should use the same home-side token/UBA translation executor.

Tasks:

1. Increase configurable IOTLB capacity:
   - default > 256 for performance tests.
   - expose via QEMU property/env.
2. Replace arbitrary hash-table eviction with LRU or clock.
3. Keep separate accounting by:
   - `tecte_tag`
   - `tid`
   - granule
   - access type
4. Add optional range translation cache for contiguous imported windows:
   - cache `{iova_base, translated_base, len, perm, tecte_tag, tid}`.
   - use only when PTW produces contiguous page/block mappings.
5. Keep invalidation exact enough:
   - all
   - by `tecte_tag`
   - by range when available

Acceptance:

1. Repeated remote access to the same page hits IOTLB.
2. Sequential remote access across contiguous pages benefits from range cache.
3. TECTE invalidation removes stale entries.
4. Translation counters prove PTW reduction.

### Phase 5: Dedicated OBMM imported-PA stress and correctness suite

Goal: validate data-plane performance without coupling to real inference.

Workload dimensions:

1. Read pattern:
   - scalar repeated same cache line
   - sequential page scan
   - random page sample
2. Write pattern:
   - scalar overwrite
   - sequential fill
   - mixed read/write
3. Size:
   - 4 KiB
   - 64 KiB
   - 2 MiB
   - 32 MiB
4. Flush mode:
   - no flush until unmap
   - periodic sync
   - explicit flush after each phase
5. Node count:
   - dual-node first
   - four/eight-node contention later

Correctness checks:

1. Home memory checksum after writeback.
2. Read-after-write visibility according to configured mode.
3. Dirty range replay after eviction.
4. Unmap flush behavior.
5. Fault injection:
   - link unavailable
   - remote UMMU translation failure
   - stale token
   - response timeout

Performance metrics:

1. guest-visible operation count
2. remote packet count
3. bytes sent/received
4. read latency distribution
5. write flush latency
6. SIM_UMMU IOTLB hit rate
7. PTW count

## 7. Implementation Notes

### 7.1 Do not optimize by bypassing UMMU

SIM_UMMU execution of UB data-path memory requests must continue to use token-aware UMMU translation. The home side is not doing decoder mapping; it is executing already-decoded remote UBA/token requests from SIM_DEC, UDMA/URMA, or later UB data-path protocols. Any shortcut that writes directly to `address_space_memory` without explicit debug/test mode would weaken the memory-access contract.

### 7.2 Keep CPU-window and DMA-path semantics distinct

There are two SIM_DEC entry points:

1. CPU imported-PA access:
   - MemoryRegion overlay
   - `sim_dec_cpu_window_read/write`
2. UBC DMA access to decoder-mapped PA:
   - `ubc_dma_read_ex/write_ex`
   - `sim_dec_lookup_by_pa`

Instrumentation and tests must report them separately.

### 7.3 Batch at page/range granularity

The useful unit is a page or dirty range, not a scalar CPU access. Any new cache/transport API should accept range operations from the start.

### 7.4 Preserve current protocol as fallback

The existing Unix socket path is valuable for debugging and should remain available:

1. no shared-memory setup required
2. simple packet capture via logs
3. easier failure injection
4. lower implementation risk

## 8. Suggested Work Order

1. Add instrumentation and a dedicated OBMM imported-PA stress CLI.
2. Add page read cache for CPU-window reads.
3. Add the explicit guest-facing sync/barrier interface, then add write-back/write-combine mode behind an explicit feature flag.
4. Add batched SIM_DEC socket messages.
5. Add shared-memory ring transport.
6. Improve UMMU IOTLB capacity/replacement and add range cache.
7. Promote optimized mode from opt-in to default only after stress correctness passes.

## 9. Open Decisions

1. Default cache mode:
   - conservative `write-through`
   - or opt-in `write-back`
2. Page size:
   - fixed 4 KiB first
   - or derive from UMMU granule
3. Transport rollout:
   - batch-over-socket first
   - or implement shared-memory ring immediately
4. Visibility contract:
   - when exactly should remote writes become visible to peer readers?
   - only after `sync/flush`, or immediately in write-through mode?
5. Failure contract:
   - should CPU load failure return zero, raise an emulated fault, or record RAS/error state?

## 10. Near-Term Acceptance Target

The next implementation milestone should not be tied to Qwen/W4/W5 inference. It should be a focused OBMM imported-PA stress suite:

```text
dual node
  -> export home region
  -> import user region with remote_uba/token
  -> map SIM_DEC
  -> run read/write stress on imported PA
  -> verify home-side bytes/checksum
  -> report SIM_DEC/UB link/SIM_UMMU counters
```

Minimum pass criteria:

1. correctness passes for read/write/flush/unmap
2. no fatal QEMU marker
3. no UMMU translation error in valid-token case
4. packet count drops after cache/batch phases
5. IOTLB hit rate and PTW count are reported
