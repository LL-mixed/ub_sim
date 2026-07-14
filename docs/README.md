# Simulator Docs

Workspace-local notes and implementation-specific design material can live here.

Current validation entry points:

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
  - next-stage plan for direct official DeepSeek V4 Flash Safetensors, FP8/FP4 execution, CPU-reference alignment, W5 2/3/8-node validation, and MTP; DS4 remains read-only and 1M context is explicitly not validated
- [plans/2026-05-15-w4-engram-phase5-performance-plan.md](plans/2026-05-15-w4-engram-phase5-performance-plan.md)
  - execution plan for W4 engram Phase 5 performance work, including profiling gates and vendor fused SIMT reuse boundaries
- [drafts/obmm_spmc_mpsc_queue_design.md](drafts/obmm_spmc_mpsc_queue_design.md)
  - draft design for SPMC and MPSC queue extensions on top of the OBMM shmem pool cacheable/NC access model
