# Experiment matrix conventions

This directory contains versioned experiment matrices, not topology scenarios.

- A matrix references topology scenarios through the host CLI `--scenario`.
- Factor domains and canonical cases are declared here; QEMU model manifests
  are generated through the P0 schema and are never hand-written beside raw
  results.
- Every matrix file uses a version suffix such as `_v1.yaml` and a strict
  schema number. Unknown fields must fail parsing.
- `obmm_remote_load_eval_v1.yaml` is the complete P3 matrix. The
  `obmm_remote_load_eval_acceptance_v1.yaml` matrix keeps the same formal
  per-case minimums and seven seeds, but selects one factor point for each
  canonical path. It verifies dispatch, evidence collection, aggregation, and
  reporting; it must not be cited as the complete performance study.
- `obmm_remote_load_policy_coarse_v1.yaml` is the completed seven-seed coarse
  policy surface. It contains 80 exact `(L,C,W)` buckets and 2,240 canonical
  runs; it is historical measured evidence, not permission to interpolate.
- `obmm_remote_load_policy_boundary_screen_v1.yaml` and
  `obmm_remote_load_policy_boundary_trace_v1.yaml` are three-seed discovery
  matrices. Together they cover 224 distinct buckets and locate adjacent
  latency winner flips. Their canonical runs passed, but three seeds do not
  satisfy the formal policy publication gate.
- `obmm_remote_load_policy_boundary_formal_v1.yaml` is the generated formal
  matrix for 70 unique flip endpoints. Its paired seven-seed campaign completed
  1,960/1,960 canonical runs with `validation.status=pass`. Published policy
  still applies only to those exact endpoints.
- The 4,942-run complete P3 campaign remains separately paused. Completion of
  the boundary matrices must not be reported as completion of the full
  jitter, tail, failure, range, and sensitivity study.
- Generated manifests, raw records, summaries, and reports belong under
  `out/`, never in this directory.
