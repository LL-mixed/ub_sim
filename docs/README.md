# Simulator Docs

Workspace-local notes and implementation-specific design material can live here.

Current validation entry points:

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
- [lingqu_db_object_service_design.md](lingqu_db_object_service_design.md)
  - detailed design for a general Lingqu DB/Object Service that manages Qwen3 weights, KV cache, hidden boundaries, runtime tensors, versions, and shmem/block payload placement before 8-node range forward
- [drafts/qwen3_0_6b_engram_obmm_simpler_8node_design.md](drafts/qwen3_0_6b_engram_obmm_simpler_8node_design.md)
  - draft design for combining CPU-side engram policy, OBMM shmem pool transport, and simpler-backed Qwen3 0.6B forward in an 8-node simulation path
- [plans/2026-05-22-paper-engram-alignment-plan.md](plans/2026-05-22-paper-engram-alignment-plan.md)
  - canonical plan for aligning repo Engram work with `Engram_paper.pdf`, including trained table construction, Memory Service artifacts, and W5 rebase boundaries
- [plans/2026-05-22-paper-engram-alignment-explainer-zh.md](plans/2026-05-22-paper-engram-alignment-explainer-zh.md)
  - Chinese explanation of the Engram paper concepts and the paper-aligned repo design plan
- [plans/2026-05-27-w5-shortpath-approximate-hidden-match-plan.md](plans/2026-05-27-w5-shortpath-approximate-hidden-match-plan.md)
  - plan for adding an opt-in approximate hidden-state match path to W5 shortpath while preserving exact-match correctness
- [plans/2026-05-15-w4-engram-phase5-performance-plan.md](plans/2026-05-15-w4-engram-phase5-performance-plan.md)
  - execution plan for W4 engram Phase 5 performance work, including profiling gates and vendor fused SIMT reuse boundaries
- [drafts/obmm_spmc_mpsc_queue_design.md](drafts/obmm_spmc_mpsc_queue_design.md)
  - draft design for SPMC and MPSC queue extensions on top of the OBMM shmem pool cacheable/NC access model
