# Dual-Node Demo Apps Implementation Plan

**Goal:** Create 3 demo apps (chat, URMA UDMA, RPC) that showcase inter-node communication in the dual-node QEMU/UB simulation.

**Architecture:** Each app is a standalone C binary statically compiled for aarch64. They reuse the same network init pattern from `urma_dp.c` (find ipourma, set static IP, install ARP). Apps are triggered by kernel cmdline flags in `init.c`.

**Tech Stack:** C (POSIX socket API, ioctl), aarch64 static cross-compilation, QEMU dual-node simulation

---

### Task 1: Create ub_chat.c — Chat Demo

**Files:**
- Create: `simulator/guest-linux/aarch64/ub_chat.c`

**Step 1: Write ub_chat.c**

A complete C source file implementing a multi-round chat between nodeA and nodeB over UDP. Key structure:

```c
/* Network init (reuse urma_dp.c pattern):
 * - cmdline_get_value("linqu_urma_dp_role", ...) to get role
 * - find_ipourma_iface() to find the interface
 * - set_ipv4_addr() for static IP (nodeA=10.0.0.1, nodeB=10.0.0.2)
 * - install_static_arp() for peer
 *
 * Startup Synchronization (P0 Fix):
 *   nodeB (server) listens on STARTUP_SYNC_PORT (18559) for "SYNC_REQ"
 *   nodeA (client) sends "SYNC_REQ" and waits for "SYNC_ACK"
 *   Both wait up to 30s for sync, fail if timeout
 *
 * Chat protocol:
 *   TX: CHAT:<role>:<text>:<seq>:<timestamp_ms>
 *   RX: parse peer message, compute latency, print
 *
 * Flow:
 *   1. Startup sync (ensure both nodes ready)
 *   2. nodeA sends seq=0..4, each time waiting for nodeB reply before next
 *   3. nodeB receives and immediately replies with its own message
 *   4. 5 rounds total
 *   5. Print per-message latency and final summary
 */
```

The implementation must include:
- `read_file()`, `cmdline_get_value()` — same as urma_dp.c
- `find_ipourma_iface()`, `iface_is_up()`, `wait_iface_ready()` — same as urma_dp.c
- `set_ipv4_addr()`, `get_local_ipv4()`, `install_static_arp()` — same as urma_dp.c
- **Startup sync helper**:
  ```c
  #define STARTUP_SYNC_PORT 18559
  #define SYNC_TIMEOUT_MS 30000
  
  /* nodeB: listen for SYNC_REQ, reply with SYNC_ACK */
  int startup_sync_server(void);
  
  /* nodeA: send SYNC_REQ, wait for SYNC_ACK */
  int startup_sync_client(void);
  ```
- Socket setup: UDP, nonblocking, SO_BINDTODEVICE, IP_PKTINFO, bind port 18556
- Chat logic:
  - **Startup sync**: nodeB starts sync server, nodeA connects and waits for ACK
  - nodeA: for seq 0..4: send `CHAT:nodeA:hello:<seq>:<ts>`, wait for reply, compute latency
  - nodeB: wait for message from nodeA, reply `CHAT:nodeB:world:<seq>:<ts>`
  - Both print `[CHAT] <role> seq=<N> "<text>" latency=<ms>`
- Summary at end: total tx/rx, avg/min/max latency
- Exit 0 on all 5 rounds complete, exit 1 on timeout (30s)
- Print `[ub_chat] pass` or `[ub_chat] fail` as final line

**Step 2: Compile test (local syntax check)**

Run:
```bash
if [[ -n "${AARCH64_LINUX_CC:-}" ]] && command -v "$AARCH64_LINUX_CC" >/dev/null 2>&1; then
  "$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra simulator/guest-linux/aarch64/ub_chat.c -o /tmp/ub_chat_test
else
  echo "skip local compile test: AARCH64_LINUX_CC not available"
fi
```
Expected: If compiler exists, compilation must succeed (no swallowed errors).

**Step 3: Commit**

```bash
git add simulator/guest-linux/aarch64/ub_chat.c
git commit -m "feat(demo): add ub_chat.c — dual-node chat demo"
```

---

### Task 2: Create ub_rpc_demo.c — RPC Demo

**Files:**
- Create: `simulator/guest-linux/aarch64/ub_rpc_demo.c`

**Step 1: Write ub_rpc_demo.c**

A complete C source file implementing structured RPC over UDP.

```c
/* RPC Protocol:
 *   Request:  RPC:<msg_id>:<op>:<payload_len>:<payload>
 *   Response: RPC_RSP:<msg_id>:<status>:<payload_len>:<payload>
 *
 * Operations:
 *   ECHO    — server echoes back payload
 *   COMPUTE — server evaluates "a OP b" (OP: +,-,*,/)
 *   STATUS  — server returns rpc_count and uptime
 *   MEMINFO — server reads /proc/meminfo, returns "MemTotal:<val> MemFree:<val>"
 *
 * Flow:
 *   nodeB = server (listen loop, respond to requests)
 *   nodeA = client (send 4 request types sequentially, validate responses)
 *
 * Port: 18557 (separate from chat=18556 and urma_dp=18555)
 */
```

Implementation must include:
- Same network init helpers as ub_chat.c
- Socket on port 18557
- RPC server (nodeB):
  - Receive loop, parse request
  - Dispatch by op: echo, compute, status, meminfo
  - Send response
  - Print each request handled
  - **Exit after receiving `SHUTDOWN` op (P1 Fix)**
  - **Max 5 ops (4 test + 1 shutdown), timeout 60s**
- RPC client (nodeA):
  - Send ECHO("hello ub rpc"), verify response matches
  - Send COMPUTE("42+58"), verify result=100
  - Send STATUS(""), print server stats
  - Send MEMINFO(""), print server memory info
  - **Send SHUTDOWN("") to signal server exit (P1 Fix)**
  - Validate each response
- Print `[ub_rpc] pass` or `[ub_rpc] fail`

**Step 2: Compile test**

Same as Task 1 — syntax check only.

**Step 3: Commit**

```bash
git add simulator/guest-linux/aarch64/ub_rpc_demo.c
git commit -m "feat(demo): add ub_rpc_demo.c — structured RPC demo"
```

---

### Task 3: Create ub_udma_demo.c — URMA UDMA Demo

**Files:**
- Create: `simulator/guest-linux/aarch64/ub_udma_demo.c`

**Step 1: Write ub_udma_demo.c**

This app demonstrates URMA resource management via ioctl to `/dev/uburma/<dev_name>`.

```c
/* URMA UDMA Demo via ioctl
 *
 * Prerequisite: uburma.ko loaded, /dev/uburma/<dev_name> exists
 *
 * Steps:
 * 1. Find uburma device: scan /sys/class/uburma/ or /dev/uburma/
 * 2. UDP handshake on port 18558: exchange EID, jetty_id, token
 * 3. Open device: /dev/uburma/<dev_name>
 * 4. UBURMA_CMD_CREATE_CTX
 * 5. UBURMA_CMD_ALLOC_JFC + ACTIVE_JFC
 * 6. UBURMA_CMD_ALLOC_JFR + ACTIVE_JFR
 * 7. UBURMA_CMD_ALLOC_JFS + ACTIVE_JFS
 * 8. UBURMA_CMD_ALLOC_JETTY + ACTIVE_JETTY
 * 9. UBURMA_CMD_REGISTER_SEG
 * 10. Exchange info via UDP, then UBURMA_CMD_IMPORT_JETTY
 * 11. UBURMA_CMD_BIND_JETTY
 * 12. Print all resource IDs
 *
 * Port: 18558
 */
```

Implementation must include:
- Same network init helpers
- Socket on port 18558 for peer info exchange
- Device discovery: scan `/sys/class/uburma/` for device names, open `/dev/uburma/<name>`
- ioctl helper:
  ```c
  static int uburma_ioctl(int fd, uint32_t cmd, void *args, uint32_t args_len) {
      struct uburma_cmd_hdr hdr;
      hdr.command = cmd;
      hdr.args_len = args_len;
      hdr.args_addr = (uint64_t)(uintptr_t)args;
      return ioctl(fd, UBURMA_CMD, &hdr);
  }
  ```
- **Error handling and resource cleanup (P1 Fix)**:
  ```c
  /* Resource tracking for cleanup on failure */
  struct udma_resources {
      int fd;                    /* device fd */
      uint32_t ctx_id;           /* context id */
      uint32_t jfc_id;           /* JFC id */
      uint32_t jfr_id;           /* JFR id */
      uint32_t jfs_id;           /* JFS id */
      uint32_t jetty_id;         /* Jetty id */
      uint32_t seg_id;           /* registered segment id */
      bool ctx_created : 1;
      bool jfc_alloc : 1;
      bool jfr_alloc : 1;
      bool jfs_alloc : 1;
      bool jetty_alloc : 1;
      bool seg_registered : 1;
  };
  
  /* Cleanup all allocated resources on failure */
  void cleanup_resources(struct udma_resources *res);
  
  /* Step execution with error handling */
  #define STEP_CHECK(step_num, name, expr) \
      do { \
          int ret = (expr); \
          if (ret < 0) { \
              fprintf(stderr, "[ub_udma] step %d: %s failed: %d\n", step_num, name, ret); \
              cleanup_resources(&res); \
              exit(1); \
          } \
          printf("[ub_udma] step %d: %s → %d\n", step_num, name, ret); \
      } while(0)
  ```
- Include a minimal subset of struct definitions from `uburma_cmd.h` needed for user-space:
  - `uburma_cmd_hdr` with magic and ioctl define
  - `uburma_cmd_create_ctx`
  - `uburma_cmd_alloc_jfc`, `uburma_cmd_active_jfc`
  - `uburma_cmd_alloc_jfr`, `uburma_cmd_active_jfr`
  - `uburma_cmd_alloc_jfs`, `uburma_cmd_active_jfs`
  - `uburma_cmd_alloc_jetty`, `uburma_cmd_active_jetty`
  - `uburma_cmd_register_seg`
  - `uburma_cmd_import_jetty`
  - `uburma_cmd_bind_jetty`
  - `uburma_cmd_query_dev_attr` (exact name from kernel header)
  - `uburma_cmd_udrv_priv`
- **ABI lock requirement**:
  - Source of truth is `simulator/guest-linux/kernel_ub/drivers/ub/urma/uburma/uburma_cmd.h`.
  - Create a dedicated user header, e.g. `simulator/guest-linux/aarch64/uburma_cmd_user_compat.h`, copied from the exact kernel header commit used to build guest modules.
  - Add `static_assert(sizeof(...))` checks on every copied struct used by demo code.
  - Add a header comment recording kernel commit hash and source path. If hash changes, this header must be reviewed/updated.
  - Keep command IDs/macros aligned exactly (`UBURMA_CMD`, `UBURMA_CMD_QUERY_DEV_ATTR`, etc.).
- Each step uses STEP_CHECK macro for consistent error handling and cleanup
- Graceful handling if uburma device not found: print skip message, exit 0
- Print `[ub_udma] pass` or `[ub_udma] fail`

**Step 2: Compile test**

Same as Task 1 — syntax check only.

**Step 3: Commit**

```bash
git add simulator/guest-linux/aarch64/ub_udma_demo.c
git commit -m "feat(demo): add ub_udma_demo.c — URMA UDMA ioctl demo"
```

---

### Task 4: Modify build_initramfs.sh — Compile and Package New Binaries

**Files:**
- Modify: `simulator/guest-linux/aarch64/build_initramfs.sh`

**Step 1: Add new source/binary variables**

After line 16 (`INIT_MANUAL_BIND_BIN=...`), add:

```zsh
CHAT_SRC="$ROOT_DIR/ub_chat.c"
CHAT_BIN="$OUT_DIR/linqu_ub_chat"
RPC_SRC="$ROOT_DIR/ub_rpc_demo.c"
RPC_BIN="$OUT_DIR/linqu_ub_rpc"
UDMA_SRC="$ROOT_DIR/ub_udma_demo.c"
UDMA_BIN="$OUT_DIR/linqu_ub_udma_demo"
```

After line 21 (`IPOURMA_MODULE=...`), add:

```zsh
UBURMA_MODULE="${UB_URMA_GUEST_MODULE:-}"
```

**Step 2: Add compilation lines**

After line 42 (`"$AARCH64_LINUX_CC" ... init_manual_bind`), add:

```zsh
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$CHAT_SRC" -o "$CHAT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$RPC_SRC" -o "$RPC_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$UDMA_SRC" -o "$UDMA_BIN"
```

**Step 3: Add binary copy lines**

After line 48 (`cp "$INSMOD_BIN" ...`), add:

```zsh
cp "$CHAT_BIN" "$INITRAMFS_DIR/bin/linqu_ub_chat"
cp "$RPC_BIN" "$INITRAMFS_DIR/bin/linqu_ub_rpc"
cp "$UDMA_BIN" "$INITRAMFS_DIR/bin/linqu_ub_udma_demo"
```

**Step 4: Add uburma.ko copy**

After the IPOURMA_MODULE block (after line 79), add:

```zsh
if [[ -n "$UBURMA_MODULE" ]]; then
  cp "$UBURMA_MODULE" "$INITRAMFS_DIR/lib/modules/uburma.ko"
fi
```

**Step 5: Commit**

```bash
git add simulator/guest-linux/aarch64/build_initramfs.sh
git commit -m "build(initramfs): add chat, rpc, udma demo binaries and uburma.ko"
```

---

### Task 5: Modify init.c — Add Demo Triggers

**Files:**
- Modify: `simulator/guest-linux/aarch64/init.c`

**Step 1: Add cmdline check functions**

After line 194 (`should_run_urma_dp_verify`), add:

```c
static bool should_run_ub_chat(void)
{
    return cmdline_has_option("linqu_ub_chat=1");
}

static bool should_run_ub_udma_demo(void)
{
    return cmdline_has_option("linqu_ub_udma_demo=1");
}

static bool should_run_ub_rpc_demo(void)
{
    return cmdline_has_option("linqu_ub_rpc_demo=1");
}
```

**Step 2: Add probe runner functions**

After line 608 (end of `run_urma_dp_probe`), add three new functions following the same fork/exec/waitpid pattern:

```c
static void run_ub_chat_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_chat failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_chat", "/bin/linqu_ub_chat", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_chat failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_chat failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub chat pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub chat fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub chat fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_udma_demo_probe(void)
{
    pid_t pid;
    int status = 0;

    /* Load uburma.ko before running UDMA demo */
    try_insmod("/lib/modules/uburma.ko");

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_udma_demo failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_udma_demo", "/bin/linqu_ub_udma_demo", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_udma_demo failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_udma_demo failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub udma demo pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub udma demo fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub udma demo fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_rpc_demo_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_rpc failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_rpc", "/bin/linqu_ub_rpc", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_rpc failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_rpc failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub rpc demo pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub rpc demo fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub rpc demo fail signal=%d\n", WTERMSIG(status));
    }
}
```

**Step 3: Add invocation in main()**

After line 893 (`run_urma_dp_probe()` block), add before `should_run_linqu_probe()`:

```c
    if (should_run_ub_chat()) {
        wait_for_ipourma_interface(30);
        run_ub_chat_probe();
    }
    if (should_run_ub_udma_demo()) {
        wait_for_ipourma_interface(30);
        run_ub_udma_demo_probe();
    }
    if (should_run_ub_rpc_demo()) {
        wait_for_ipourma_interface(30);
        run_ub_rpc_demo_probe();
    }
```

**Step 4: Commit**

```bash
git add simulator/guest-linux/aarch64/init.c
git commit -m "feat(init): add cmdline triggers for chat, rpc, udma demo apps"
```

---

### Task 6: Create run_ub_dual_node_demo.sh — Orchestrator

**Files:**
- Create: `simulator/guest-linux/aarch64/run_ub_dual_node_demo.sh`

**Step 1: Write run_ub_dual_node_demo.sh**

Based on `run_ub_dual_node_urma_dataplane_workload_test.sh` (same QEMU launch, FM link wait, entity readiness, log validation logic), but with these changes:

- APPEND_EXTRA includes demo flags:
  ```
  APPEND_EXTRA="linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_force_ubase_bind=1 linqu_ub_chat=1 linqu_ub_rpc_demo=1"
  ```
  Note: `linqu_ub_udma_demo=1` is NOT included by default since it requires uburma.ko. Add it manually if uburma.ko is available.

- Validation functions for each demo (must propagate failure with explicit return):
  ```zsh
  validate_chat_log() {
      local node_name="$1"
      local log_file="$2"
      assert_log_has "$log_file" "\\[ub_chat\\] pass" "${node_name} chat pass" || return 1
      assert_log_absent "$log_file" "\\[ub_chat\\] fail" "${node_name} chat fail" || return 1
  }

  validate_rpc_log() {
      local node_name="$1"
      local log_file="$2"
      assert_log_has "$log_file" "\\[ub_rpc\\] pass" "${node_name} rpc pass" || return 1
      assert_log_absent "$log_file" "\\[ub_rpc\\] fail" "${node_name} rpc fail" || return 1
  }

  validate_udma_log() {
      local node_name="$1"
      local log_file="$2"
      assert_log_has "$log_file" "\\[ub_udma\\] pass" "${node_name} udma pass" || return 1
      assert_log_absent "$log_file" "\\[ub_udma\\] fail" "${node_name} udma fail" || return 1
  }
  ```

- Add kernel health gates (hard fail):
  ```zsh
  validate_kernel_health_log() {
      local node_name="$1"
      local log_file="$2"
      assert_log_absent "$log_file" "WARNING: CPU:" "${node_name} kernel warning" || return 1
      assert_log_absent "$log_file" "Call trace:" "${node_name} stacktrace" || return 1
      assert_log_absent "$log_file" "Kernel panic - not syncing" "${node_name} panic" || return 1
  }
  ```

- Log file naming: `ub_nodeA.demo.<iter>.log` / `ub_nodeB.demo.<iter>.log`
- pid files: `ub_nodeA.demo.<iter>.pid` / `ub_nodeB.demo.<iter>.pid`
- Wait for `[init] ub chat pass` / `[init] ub rpc demo pass` instead of `[init] urma dataplane pass`
- **Add global timeout and full cleanup (P2 Fix)**:
  ```zsh
  MAX_RUNTIME="${MAX_RUNTIME:-300}"  # 5 minutes default
  MAIN_PID=$$

  cleanup_all_demo_pid_files() {
      local pid_file
      for pid_file in "$OUT_DIR"/ub_nodeA.demo.*.pid "$OUT_DIR"/ub_nodeB.demo.*.pid; do
          cleanup_pid "$pid_file"
      done
  }

  timeout_watchdog() {
      local timeout_sec="$1"
      sleep "$timeout_sec"
      echo "global timeout ${timeout_sec}s reached, terminating test" >&2
      kill -TERM "$MAIN_PID" 2>/dev/null || true
  }

  timeout_watchdog "$MAX_RUNTIME" &
  WATCHDOG_PID=$!
  trap 'kill "$WATCHDOG_PID" 2>/dev/null || true; cleanup_all_demo_pid_files' EXIT INT TERM
  ```
  Note: `timeout_watchdog` runs in background. It must signal `MAIN_PID` (not `exit` in watchdog subshell), otherwise the main script may continue waiting after timeout.

- Important: log validation calls must be directly checked (or `|| return 1`). Do not rely on `set -e` inside `if run_iteration ...` blocks.
- In `run_iteration()`, enforce validation in this order and hard-fail on first error:
  ```zsh
  validate_chat_log "nodeA" "$nodea_log" || return 1
  validate_chat_log "nodeB" "$nodeb_log" || return 1
  validate_rpc_log "nodeA" "$nodea_log" || return 1
  validate_rpc_log "nodeB" "$nodeb_log" || return 1
  validate_kernel_health_log "nodeA" "$nodea_log" || return 1
  validate_kernel_health_log "nodeB" "$nodeb_log" || return 1
  ```

**Step 2: Make executable**

```bash
chmod +x simulator/guest-linux/aarch64/run_ub_dual_node_demo.sh
```

**Step 3: Commit**

```bash
git add simulator/guest-linux/aarch64/run_ub_dual_node_demo.sh
git commit -m "feat(demo): add dual-node demo orchestrator script"
```

---

### Task 7: Build and Smoke Test

**Prerequisites:**
- AARCH64_LINUX_CC set (e.g., `aarch64-linux-gnu-gcc`)
- QEMU binary built with UB support
- Kernel image built
- All required .ko modules available

**Step 1: Build initramfs with demo apps**

```bash
cd simulator/guest-linux/aarch64
# Set environment variables for your build
export AARCH64_LINUX_CC=aarch64-linux-gnu-gcc
# Run build
./build_initramfs.sh
```

Expected: `out/initramfs.cpio.gz` created successfully, containing:
- `bin/linqu_ub_chat`
- `bin/linqu_ub_rpc`
- `bin/linqu_ub_udma_demo`

Verify:
```bash
ls out/initramfs/bin/ | grep linqu_ub
```

**Step 2: Run demo (chat + rpc, without udma)**

```bash
./run_ub_dual_node_demo.sh
```

Expected: Both nodes boot, FM links come up, chat and rpc demos pass, logs show `[init] ub chat pass` and `[init] ub rpc demo pass`.
And logs must not include `WARNING: CPU:`, `Call trace:`, or `Kernel panic - not syncing`.

**Step 3: Run with UDMA demo (if uburma.ko available)**

```bash
UB_URMA_GUEST_MODULE=/path/to/uburma.ko \
APPEND_EXTRA="linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_force_ubase_bind=1 linqu_ub_chat=1 linqu_ub_rpc_demo=1 linqu_ub_udma_demo=1" \
./run_ub_dual_node_demo.sh
```

Expected: All 3 demos pass.

**Step 4: Commit any fixes**

If any issues found during testing, fix and commit with descriptive messages.

---

### Task 8: Final Commit — Clean Up

**Step 1: Verify all files are committed**

```bash
git status
git log --oneline -10
```

**Step 2: Squash or organize commits if needed**

Review commit history and ensure clean, logical commits.

---

## Review Fixes Applied

This plan was reviewed and the following issues were fixed:

### 🔴 P0: Node Startup Synchronization
**Problem:** Chat demo had race condition where nodeA could send before nodeB was ready.
**Fix:** Added `STARTUP_SYNC_PORT` (18559) for explicit startup handshake:
- nodeB listens for "SYNC_REQ", replies with "SYNC_ACK"
- nodeA sends "SYNC_REQ" and waits for ACK before proceeding
- Both have 30s timeout for sync

### 🟠 P1: RPC SHUTDOWN Mechanism
**Problem:** RPC server (nodeB) exit condition was unclear.
**Fix:** 
- Client (nodeA) explicitly sends `SHUTDOWN` op after 4 test ops
- Server exits cleanly upon receiving SHUTDOWN
- Added 60s max timeout for server

### 🟠 P1: UDMA Error Handling
**Problem:** 12-step UDMA resource allocation had no cleanup on failure.
**Fix:**
- Added `struct udma_resources` to track allocated resources
- Added `cleanup_resources()` function for proper cleanup
- Added `STEP_CHECK` macro for consistent error handling:
  ```c
  STEP_CHECK(step_num, name, expr)
  ```
- Each step failure triggers cleanup and exits with error

### 🟡 P2: Script Timeout
**Problem:** No global timeout, could hang indefinitely.
**Fix:** Added `MAX_RUNTIME=300` watchdog + `trap` cleanup, and watchdog now signals `MAIN_PID` on timeout (instead of `exit` in subshell), ensuring the main script is actually interrupted and cleaned up.

### 🔴 P0: False Pass Risk in Log Validation
**Problem:** Validation functions could print errors without failing the iteration when called under `if run_iteration ...`.
**Fix:** All `assert_log_*` checks now require explicit failure propagation (`|| return 1`), and kernel health gates were added.

### 🟠 P1: URMA ioctl ABI Drift Risk
**Problem:** User-space manual struct copies can silently drift from kernel ABI.
**Fix:** Added ABI lock rules: fixed source header path, command name alignment (`QUERY_DEV_ATTR`), struct `static_assert`, and kernel commit hash binding for copied header.
