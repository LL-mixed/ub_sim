# Simulator Docs

Workspace-local notes and implementation-specific design material can live here.

Current validation entry points:

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
