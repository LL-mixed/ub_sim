# Simulator Docs

Workspace-local notes and implementation-specific design material can live here.

Current async-load architecture and validation entry points:

- [2026-09-03-async-load-overall-solution-introduction.md](2026-09-03-async-load-overall-solution-introduction.md)
  - current source-owned void-response contract for source-local and
    remote-wire triggers, tokenless runnable-yield, fresh-transaction replay,
    Cacheable fill, and Normal NC scalar completion
- [2026-09-05-source-owned-void-response-validation-report.md](2026-09-05-source-owned-void-response-validation-report.md)
  - QEMU unit evidence and the six-case source-local, remote-wire, dual-trigger,
    Normal NC, and Normal Cacheable two-node acceptance matrix
- [plans/2026-09-05-ub-void-response-predicate-policy-design.md](plans/2026-09-05-ub-void-response-predicate-policy-design.md)
  - configurable latency, jitter, fault/recovery predicate and source-UBC
    exactly-once transaction arbitration
- [2026-09-02-async-ldr-latest-design-implementation.md](2026-09-02-async-ldr-latest-design-implementation.md)
  - earlier architecture decision record for the ESR/SVC/WFE/ERET and
    memory-type exploration; superseded where it conflicts with the 2026-09-05
    source-owned tokenless contract
- [plans/async-load-implementation-summary.md](plans/async-load-implementation-summary.md)
  - source, CLI, evidence, and document navigation for control ABI 4, event
    ABI 3, both scheduler modes, and both memory types
- [plans/2026-09-02-normal-nc-replay-plt-svc-eret-design.md](plans/2026-09-02-normal-nc-replay-plt-svc-eret-design.md)
  - historical Normal Non-cacheable PLT/SVC/WFE design and evidence; the
    2026-09-05 contract removes PLT from the void-response path
- [plans/2026-09-02-normal-cacheable-void-response-esr-cq-validation-design.md](plans/2026-09-02-normal-cacheable-void-response-esr-cq-validation-design.md)
  - historical Normal Cacheable CQ/IRQ wakeup design and evidence; the
    2026-09-05 contract uses runnable-yield and drops the old completion
- [plans/2026-09-01-obmm-kernel-task-remote-load-poc.md](plans/2026-09-01-obmm-kernel-task-remote-load-poc.md)
  - Linux-task scheduler path shared by Normal Cacheable and Normal NC
- [plans/async-load-abi-v3-kernel-free-event-ring.md](plans/async-load-abi-v3-kernel-free-event-ring.md)
  - EL0-owned event-ring processing, current SVC context resume, WFE/IRQ idle
    wakeup, and the exact boundary of the historical `kernel-free` label
- [2026-08-20-gva-gsva-upcall-coroutine-hardware-mechanisms.md](2026-08-20-gva-gsva-upcall-coroutine-hardware-mechanisms.md)
  - current simulator-to-silicon component mapping: Cacheable uses ordinary
    cache/MSHR/fill, Normal NC uses requester-UBC NC PLT, and legacy HLT
    assists are excluded from the current contract

Current validation entry points:

- [plans/2026-08-25-sim-console-unified-control-plane.md](plans/2026-08-25-sim-console-unified-control-plane.md)
  - unified Web and CLI control plane for the simulator demo catalog, typed
    launch configuration, 2/4/8-node topology state, process and node logs, run
    history, and lifecycle controls
- [2026-07-16-w5-deepseek-v4-flash-official-first-token-report.md](2026-07-16-w5-deepseek-v4-flash-official-first-token-report.md)
  - stage-5 evidence for the official position-0 prompt `[1]`, all 43 transformer layers, A5 matrix/vector production dispatch, bounded checkpoint reads/caches, exact terminal logits and top-1 token 294
- [2026-07-14-w5-deepseek-v4-flash-official-routed-expert-production-report.md](2026-07-14-w5-deepseek-v4-flash-official-routed-expert-production-report.md)
  - stage-4 evidence for official packed E2M1/UE8M0 routed experts, hash and learned routing, clamped SwiGLU, top-6 combination, selected-only loading, and bounded expert-cache behavior
- [2026-07-14-w5-deepseek-v4-flash-official-linear-production-report.md](2026-07-14-w5-deepseek-v4-flash-official-linear-production-report.md)
  - stage-3 evidence for official FP8 E4M3/UE8M0 A5 MX execution, dynamic activation quantization, BF16/F32 output, representative attention/grouped/shared linears, and the checkpoint's BF16 output head
- [2026-07-14-w5-deepseek-v4-flash-official-reference-oracle-report.md](2026-07-14-w5-deepseek-v4-flash-official-reference-oracle-report.md)
  - stage-2 evidence for independent official FP8/FP4/UE8M0 scalar decoding, dynamic activation quantization, operator checksums, and complete position-0 layer reference forward
- [2026-07-14-w5-deepseek-v4-flash-official-checkpoint-loader-report.md](2026-07-14-w5-deepseek-v4-flash-official-checkpoint-loader-report.md)
  - stage-1 evidence for direct official config/index/46-shard schema validation, positioned tensor/expert slice reads, bounded caches, checksums, and fail-closed loader tests
- [qwen3_simpler_build_output_validation.md](qwen3_simpler_build_output_validation.md)
  - validates Qwen3 0.6B/14B L2 and L3 generation on a simpler-backed device using the packaged `build_output/Qwen*` programs
- [reports/2026-04-14-four-node-matrix-validation.md](2026-04-14-four-node-matrix-validation.md)
  - current four-node full-mesh matrix status for `chat`, `rpc`, `udma`, and `obmm-pool`
- [reports/2026-04-15-eight-node-matrix-validation.md](2026-04-15-eight-node-matrix-validation.md)
  - current eight-node full-mesh matrix status for `chat`, `rpc`, `udma`, and `obmm-pool`
- [2026-04-15-ubsim-eight-node-final-validation.md](2026-04-15-ubsim-eight-node-final-validation.md)
  - final standalone `ub_sim.git` eight-node confirmation after artifact freshness/rebuild self-check fixes
- [sim_ub_eight_node_full_mesh_design.md](sim_ub_eight_node_full_mesh_design.md)
  - eight-node scale-up design notes, including configurable `port_num` and FM-owned `EID/CNA` constraints
- [sim_gva_simulation_design.md](sim_gva_simulation_design.md)
  - design for adding explicit GVA simulation semantics on top of the current OBMM/SIM_DEC/QEMU UB Link path
- [sim_gsva_shared_virtual_address_design.md](sim_gsva_shared_virtual_address_design.md)
  - design for a GSVA mode where OBMM shmem ranges use identical user VA, public UBA, and home VA across nodes
  - includes bootstrap dependency on existing OBMM bootstrap and manager queue bootstrap flow
- [2026-08-20-gva-gsva-upcall-coroutine-hardware-mechanisms.md](2026-08-20-gva-gsva-upcall-coroutine-hardware-mechanisms.md)
  - audited GVA/GSVA/async-load hardware breakdown with memory-type state
    ownership, simulator-to-silicon boundaries, and joint-integration gaps
- [2026-06-24-w5-gva-gsva-dataplane-benefit-report.md](2026-06-24-w5-gva-gsva-dataplane-benefit-report.md)
  - host-core dataplane microbenchmark benefit report for W5 GVA/GSVA, including expanded legacy PA-to-UBA resolver baselines (`linear`, `direct`, `indexed`, `cached`)
- [w5_test_env_inventory.md](w5_test_env_inventory.md)
  - inventory and naming rule for W5 validation, test, report, and check environment variables; these variables use the `SIM_W5_TEST_*` namespace
- [w5_manual_serving_run.md](w5_manual_serving_run.md)
  - current manual entry for 8-node W5 stream inference and sequential serving request submission
- [w5_script_inventory.md](w5_script_inventory.md)
  - W5 script surface inventory separating manual entry, internal runtime, validation, maintenance, and compatibility wrappers
- [lingqu_db_object_service_design.md](lingqu_db_object_service_design.md)
  - detailed design for a general Lingqu DB/Object Service that manages Qwen3 weights, KV cache, hidden boundaries, runtime tensors, versions, and shmem/block payload placement before 8-node range forward
- [lingqu_datasystem_review.md](lingqu_datasystem_review.md)
  - Chinese cross-repository review of Lingqu DataSystem, covering the original four-service model, current Object/Memory Service architecture, implementation map, maturity, gaps, and SVG diagrams
- [2026-08-14-ub-sim-lingqu-datasystem-poc-status-gap.md](2026-08-14-ub-sim-lingqu-datasystem-poc-status-gap.md)
  - audited current-state report for the relationship between `ub_sim` and Lingqu DataSystem, completed capabilities, evidence boundaries, full-PoC gaps, risks, and an estimated delivery path
- [plans/2026-08-29-lingqu-shmem-pto-ub-gm-direct-access-design.md](plans/2026-08-29-lingqu-shmem-pto-ub-gm-direct-access-design.md)
  - implemented architecture, formal evidence, and remaining plan for materializing
    `lingqu_shmem_memref` as PTO `AddressSpace::UB_GM`, reusing the existing
    QEMU UBC → sim-qemu → ChipBackend → Simpler/PTO dispatch path and issuing
    direct `TLOAD/TSTORE` through a generic QEMU UB GM memory interface without
    payload staging; includes generic 2/8-node vector gates and the 2026-09-02
    Qwen3-0.6B W5 Memory Service hidden-state 2/8-node pipeline with in-place
    publish; `sim_npu`, GVA, and GSVA remain experimental, optional, and
    default-disabled outside the acceptance path
- [plans/2026-08-11-obmm-remote-load-coroutine-feasibility-design.md](plans/2026-08-11-obmm-remote-load-coroutine-feasibility-design.md)
  - historical feasibility design comparing submit/await and the original
    direct-EL0 async-load concept; current memory-type state ownership lives in
    the 2026-09-02 design documents
- [plans/p0-baseline-latency-model-detailed-design.md](plans/p0-baseline-latency-model-detailed-design.md)
  - implementation-level P0 design for four synchronous baselines, strong scenario configuration, deterministic QEMU virtual-time latency/failure injection, three-clock observation, CLI, and reproducibility gates
- [plans/p1-split-phase-backend-detailed-design.md](plans/p1-split-phase-backend-detailed-design.md)
  - implementation-level split-phase backend design for 64 parent requests, SIM_DEC child aggregation, bounded result ownership, generation-safe test/submit-await/async-load sinks, terminal races, conformance CLI, and tests
- [plans/submit-await-detailed-design.md](plans/submit-await-detailed-design.md)
  - implementation-level submit/await design for the independent OBMM async endpoint, 64-byte SQ/CQ ABI, registered destination buffers, generation-safe futures, AArch64 EL0 stackful coroutines, CLI, and tests
- [plans/async-load-coroutine-scheduler-detailed-design.md](plans/async-load-coroutine-scheduler-detailed-design.md)
  - historical ABI v2 direct-EL0 coroutine design and producer/consumer
    evidence; it does not define the current SVC/WFE replay-only ABI
- [plans/2026-09-01-obmm-kernel-task-remote-load-poc.md](plans/2026-09-01-obmm-kernel-task-remote-load-poc.md)
  - ESR_EL1 remote-pending extension, Linux task waitqueue scheduling,
    CQ/IRQ completion, Cacheable fill replay, NC-PLT replay, and two-node results
- [plans/2026-09-01-obmm-el0-coroutine-vs-kernel-task-trace-off.md](plans/2026-09-01-obmm-el0-coroutine-vs-kernel-task-trace-off.md)
  - historical 30-case paired trace-off comparison from the pre-SVC/WFE
    artifact; current-revision performance requires a fresh campaign
- [plans/p3-comparative-evaluation-detailed-design.md](plans/p3-comparative-evaluation-detailed-design.md)
  - implementation-level P3 design for scalar/range/transparency comparison bands, schedule-ahead isolation, fairness and statistics rules, invalidation gates, CLI, evidence artifacts, and break-even reporting
- [plans/2026-08-13-obmm-p3-performance-evaluation.md](plans/2026-08-13-obmm-p3-performance-evaluation.md)
  - audited ABI v2 performance history, completed coarse/fine matrices, and
    the paused 4,942-case matrix; current ABI requires a new campaign
- [plans/2026-08-17-obmm-runtime-policy-selection.md](plans/2026-08-17-obmm-runtime-policy-selection.md)
  - formal QEMU 7-seed sync/submit-await/async-load policy, completed 1,960-run fine-grained boundary validation, native Arm64 path-tax calibration, empty-ready-queue sync fast path, L/C/W deployment prior, evidence provenance, and remaining runtime-integration targets
- [plans/2026-08-17-obmm-async-load-patch-replay-comparison-design.md](plans/2026-08-17-obmm-async-load-patch-replay-comparison-design.md)
  - historical patch/replay comparison and exact-once evidence; patch has
    exited the current replay-only contract
- [plans/p4-userfaultfd-baseline-detailed-design.md](plans/p4-userfaultfd-baseline-detailed-design.md)
  - implementation-level P4 design for the standard userfaultfd MISSING baseline, separate OBMM source and anonymous shadow ranges, handler-vCPU costs, page failure semantics, CLI, and tests
- [plans/2026-08-12-obmm-remote-load-coroutine-implementation-validation.md](plans/2026-08-12-obmm-remote-load-coroutine-implementation-validation.md)
  - implemented phase status, 49-case formal acceptance, 4/8-node scale-out, completed coarse/fine policy evidence, QEMU artifact identity, focused/full-suite audit, and the separately paused full sensitivity boundary
- [mem_service_independent_deployment_assessment.md](mem_service_independent_deployment_assessment.md)
  - assessment of whether `mem_service` can be independently released/deployed, current component capabilities, blockers, and the service-productization plan for LLM serving and pretraining integration
- [mem_service_implementation_summary.md](mem_service_implementation_summary.md)
  - implementation summary for `mem_service`, including current service capabilities, serving/pretraining integration, release/deployment gates, validation status, and remaining production-certification gaps
- [drafts/qwen3_0_6b_engram_obmm_simpler_8node_design.md](drafts/qwen3_0_6b_engram_obmm_simpler_8node_design.md)
  - draft design for combining CPU-side engram policy, OBMM shmem pool transport, and simpler-backed Qwen3 0.6B forward in an 8-node simulation path
- [plans/2026-05-22-paper-engram-alignment-plan.md](plans/2026-05-22-paper-engram-alignment-plan.md)
  - canonical plan for aligning repo Engram work with `Engram_paper.pdf`, including trained table construction, Memory Service artifacts, and W5 rebase boundaries
- [plans/2026-05-22-paper-engram-alignment-explainer-zh.md](plans/2026-05-22-paper-engram-alignment-explainer-zh.md)
  - Chinese explanation of the Engram paper concepts and the paper-aligned repo design plan
- [plans/2026-05-27-w5-shortpath-approximate-hidden-match-plan.md](plans/2026-05-27-w5-shortpath-approximate-hidden-match-plan.md)
  - plan for adding an opt-in approximate hidden-state match path to W5 shortpath while preserving exact-match correctness
- [plans/2026-06-25-mem-service-independent-service-plan.md](plans/2026-06-25-mem-service-independent-service-plan.md)
  - implementation and evaluation plan for turning `mem_service` into an independently releasable/deployable service for LLM serving and pretraining integration
- [plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md](plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md)
  - active plan for direct official DeepSeek V4 Flash Safetensors; stages 1 through 5 are complete, and stage 6 now has stateful official range execution plus the fail-closed W5 profile/CLI; token text metadata was fixed and a real 2-node 1-step run passed, while the 2/3/8-node 4/8-step matrix and MTP remain incomplete; DS4 remains read-only and 1M context is explicitly not validated
- [plans/2026-05-15-w4-engram-phase5-performance-plan.md](plans/2026-05-15-w4-engram-phase5-performance-plan.md)
  - execution plan for W4 engram Phase 5 performance work, including profiling gates and vendor fused SIMT reuse boundaries
- [drafts/obmm_spmc_mpsc_queue_design.md](drafts/obmm_spmc_mpsc_queue_design.md)
  - draft design for SPMC and MPSC queue extensions on top of the OBMM shmem pool cacheable/NC access model
