# Dual-Node Demo Apps Design

Date: 2026-04-10

## Goal

Create 3 demonstration applications that showcase inter-node communication
in the dual-node QEMU/UB simulation environment. Each app runs on both nodeA
and nodeB, communicating via the ipourma virtual network interface.

## Files

### New Files

| File | Purpose |
|------|---------|
| `simulator/guest-linux/aarch64/ub_chat.c` | Chat-style multi-round messaging demo |
| `simulator/guest-linux/aarch64/ub_rdma_demo.c` | URMA RDMA ioctl operations demo |
| `simulator/guest-linux/aarch64/ub_rpc_demo.c` | Structured RPC request-response demo |
| `simulator/guest-linux/aarch64/run_ub_dual_node_demo.sh` | Orchestrator script for all 3 demos |

### Modified Files

| File | Change |
|------|--------|
| `simulator/guest-linux/aarch64/init.c` | Add cmdline flags for 3 demos, load uburma.ko |
| `simulator/guest-linux/aarch64/build_initramfs.sh` | Compile and package new binaries + uburma.ko |

## Demo 1: ub_chat.c

**cmdline flag**: `linqu_ub_chat=1`

Network init reuses urma_dp.c pattern (find ipourma, set static IP, install
static ARP).

Protocol format:
```
CHAT:<role>:<text>:<seq>:<timestamp_ms>
```

Flow:
1. Both nodes init network (nodeA=10.0.0.1, nodeB=10.0.0.2)
2. nodeA sends `CHAT:nodeA:hello:N:<ts>`, nodeB receives and replies
3. 5 rounds of alternating messages
4. Each message prints: `[CHAT] <role> seq=<N> "<text>" latency=<ms>`
5. Summary: tx/rx counts, avg/min/max latency
6. Exit 0 on success, exit 1 on failure (timeout after 30s)

## Demo 2: ub_rdma_demo.c

**cmdline flag**: `linqu_ub_rdma_demo=1`
**Dependency**: uburma.ko must be loaded (creates /dev/uburma/<dev_name>)

Step-by-step URMA resource lifecycle via ioctl:

1. **UDP handshake**: exchange EID, token, and device name with peer
2. **Query device**: UBURMA_CMD_QUERY_DEV_ATTR → print device capabilities
3. **Create context**: UBURMA_CMD_CREATE_CTX
4. **Create JFC** (completion queue): ALLOC_JFC → ACTIVE_JFC
5. **Create JFR** (receive endpoint): ALLOC_JFR → ACTIVE_JFR
6. **Create JFS** (send endpoint): ALLOC_JFS → ACTIVE_JFS
7. **Create Jetty**: ALLOC_JETTY → ACTIVE_JETTY
8. **Register memory segment**: REGISTER_SEG
9. **Cross-node import**: UDP exchange of EID/id, then IMPORT_JETTY
10. **Bind Jetty**: BIND_JETTY
11. Print all resource IDs and step results

Each step prints success/failure. Overall PASS only if all steps succeed.

## Demo 3: ub_rpc_demo.c

**cmdline flag**: `linqu_ub_rpc_demo=1`

RPC protocol over UDP:

```
Request:  RPC:<msg_id>:<op>:<payload_len>:<payload>
Response: RPC_RSP:<msg_id>:<status>:<payload_len>:<payload>
```

Operations:
- **ECHO**: server echoes back the payload
- **COMPUTE**: server evaluates `a <op> b` (op: +,-,*,/)
- **STATUS**: server returns its stats (rpcs served, uptime)
- **MEMINFO**: server reads /proc/meminfo and returns summary

Flow:
1. nodeB = RPC server (listen and respond)
2. nodeA = RPC client (sends 4 types of requests sequentially)
3. nodeA validates each response
4. Both print RPC statistics summary

## Integration

### init.c changes

New cmdline flags and corresponding probe functions:

```
linqu_ub_chat=1       → run_ub_chat_probe()
linqu_ub_rdma_demo=1  → run_ub_rdma_demo_probe() + load uburma.ko
linqu_ub_rpc_demo=1   → run_ub_rpc_probe()
```

Execution order: after `run_urma_dp_probe()`, before `run_probe()`.

### build_initramfs.sh changes

Compile new sources:
```
ub_chat.c    → linqu_ub_chat
ub_rdma_demo.c → linqu_ub_rdma_demo
ub_rpc_demo.c  → linqu_ub_rpc
```

Copy to initramfs/bin/. Support UB_URMA_GUEST_MODULE env var for uburma.ko.

### run_ub_dual_node_demo.sh

Based on run_ub_dual_node_urma_dataplane_workload_test.sh.
APPEND_EXTRA includes all 3 demo flags.
Validates PASS markers for each demo in both node logs.
