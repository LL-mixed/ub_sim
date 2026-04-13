# UDMA Emulation Implementation Design

Date: 2026-04-08
Status: Approved
Source: sim_ub_udma_emulation_design.md
Target: simulator/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c, include/hw/ub/ub_ubc.h

## Phase A: Interface Alignment (P0)

### A1. Fix class code
- ub_ubc.h:207: UBC_CLASS_CODE 0x0 -> 0x0002

### A2. Unify mailbox sub-opcode definitions
- Add missing #defines: MODIFY_JFC(0x25), QUERY_JFC(0x26), DESTROY_JFC(0x27),
  MODIFY_JFR(0x55), QUERY_JFR(0x56), DESTROY_JFR(0x57)
- Replace hardcoded sub-opcodes in ubc_handle_post_mb() switch with named constants
- Clean up duplicate/incorrect entries (0x34, 0x44, etc.)

### A3. Add boundary comments
- Document CMDQ/Mailbox/CtrlQ call relationships

## Phase B: Control Plane Closure (P1)

### B1. JFS state machine
- Add jetty_state field (RESET/READY/ERROR/SUSPEND)
- CREATE -> READY, DESTROY -> RESET, MODIFY -> update fields, QUERY -> fill state/PI/CI

### B2. JFC lifecycle
- Full CREATE/MODIFY/QUERY/DESTROY

### B3. JFR lifecycle
- Full CREATE/MODIFY/QUERY/DESTROY

## Phase C: Data Plane Enhancement (P1)

### C1. WQE opcode dispatch
- Add WRITE/READ/CAS/FAA constants and dispatch framework

### C2. CQE field completion
- Phase bit flip, error codes, wqe_idx/byte_cnt/status

### C3. RQ/CQ boundary conditions
- Empty queue, overflow, length mismatch protections

## Phase D: Semantic Enhancement (P2)

### D1. Token/Access validation
- Minimal observation-based checking

### D2. Error CQE generation
- Error CQEs on parse/DMA/queue failures

### D3. Observability
- Convert fprintf(stderr) to qemu_log, add trace points
