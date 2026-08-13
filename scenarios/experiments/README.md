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
- Generated manifests, raw records, summaries, and reports belong under
  `out/`, never in this directory.
