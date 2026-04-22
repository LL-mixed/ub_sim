# Eight-Node Matrix Validation

## Status

Current headless eight-node full-mesh validation status:

- `smoke`: pass
- `chat`: pass
- `rpc`: pass
- `rdma`: pass
- `obmm-pool`: pass

## Evidence

- `smoke`
  - run: `2026-04-14_22-46-13_smoke8_17787_headless8`
- `chat`
  - run: `2026-04-14_22-55-40_chat8_9825_headless8`
  - report: `guest-linux/aarch64/out/eight_node_chat_matrix.latest.txt`
- `rpc`
  - run: `2026-04-14_23-24-57_rpc8_3369_headless8`
  - report: `guest-linux/aarch64/out/eight_node_rpc_matrix.latest.txt`
- `rdma`
  - run: `2026-04-15_08-15-50_rdma8_25223_headless8`
  - report: `guest-linux/aarch64/out/eight_node_rdma_matrix.latest.txt`
- `obmm-pool`
  - run: `2026-04-15_10-19-32_obmmpool8_75_headless8`
  - report: `guest-linux/aarch64/out/eight_node_obmm_pool.latest.txt`

## Notes

- Cluster identity is now treated as FM-owned state:
  - unique `EID` per node
  - unique primary `CNA` per node
- The eight-node `obmm` path is pool-based, not pairwise:
  - each node exports one slot
  - each node imports every remote slot
  - round barriers use explicit `TURN/ACK/COMMIT` control messages
  - pass requires all nodes to complete every round
