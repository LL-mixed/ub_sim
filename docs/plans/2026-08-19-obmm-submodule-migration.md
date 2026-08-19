# OBMM Submodule Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 引入 openeuler/obmm submodule（pin atomgit 53011ee），将 guest-linux 与 mem_service 的基础 OBMM export/import 封装改为基于 libobmm 的适配层（调用方零改动），sim 扩展保留本地。

**Architecture:** 三层——L0 libobmm（submodule，一字不改）、L1 obmm_common.h 适配层（签名不变，内部调 libobmm，sim priv v1/v2 blob 经 `desc->priv` 透传）、L2 sim 扩展（bootstrap/GSVA/窗口/网络助手，不动）。链接时用 sim 版 `vendor_adaptor` 替换硬件版。设计文档：`docs/plans/2026-08-19-obmm-submodule-migration-design.md`。

**Tech Stack:** C (aarch64-linux-gnu-gcc / host cc)、GNU ld `--wrap`、Python unittest、git submodule。

**执行约束（用户规则）:** 逐文件小步修改（Edit 工具），禁止 shell 脚本批量改码；每个任务后跑对应测试；commit message 英文、无 Co-Authored-By；不 push（等用户指示）。

---

## 关键事实（执行者必读）

- obmm_common.h 的 includer（**恰 11 个 app**）：
  `gsva_coh_test gsva_lifecycle_test gsva_query gva_direct gva_manager npu_gsva_test obmm_coh_test obmm_dataplane_microbench obmm_gsva obmm_import_stress ssd_gsva_test`
  （`apps/obmm_queue`、`serving_control`、`ub_obmm_pool` 只用 obmm_queue/pool_helpers，**不在范围**；`init.c` 不含 obmm_common.h，不动。）
- `build_initramfs.sh` 对部分 app 有**内联编译行**（已确认 `obmm_import_stress/obmm_dataplane_microbench/gva_direct/obmm_gsva/gva_manager`，执行时须 grep 全部 11 个名字），每行都要加 libobmm 接线。
- `obmm_mem_desc.priv` 是 flexible array：**不能赋值指针**，必须堆分配 desc 并 `memcpy(desc->priv, blob, len)`。
- 内核旧路径 `cmd.numa_id=0`（memset 所致）；libobmm 传 `numa=NULL` 会写 `NUMA_NO_NODE(-1)`——**必须传指向 0 的指针**保持等价。
- 旧 export `cmd.length=1`；libobmm 写 `cmd.length=OBMM_MAX_LOCAL_NUMA_NODES` 且整组 size 拷贝（仅 size[0] 非零）。Task 5 必须先验证内核 export 循环跳过 `size[i]==0`，否则 export 保留 raw ioctl（预案）。
- mem_service 拷贝与 ub_sim 版**仅两行差异**（kernel_ub include 路径），同步时保持。
- mem_service `test_repository_contract.py` 硬编码 VENDORED 校验和，改 `common/obmm_common.h` 后须同步更新两处。
- 工具链：本机有 `aarch64-linux-gnu-gcc`、`cc`。
- 测试入口：ub_sim `python3 -m unittest discover guest-linux/aarch64/tests`；构建 `cd guest-linux/aarch64 && ./scripts/run_ub_app_build_matrix.sh`；mem_service standalone `python3 -m unittest discover tests`。

---

### Task 1: submodule 契约测试（先写，失败）

**Files:**
- Create: `guest-linux/aarch64/tests/test_obmm_submodule_contract.py`

**Step 1:** 写契约测试：

```python
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GITMODULES = ROOT / ".gitmodules"
LIBOBMM_MK = ROOT / "guest-linux/aarch64/common/libobmm.mk"
SIM_ADAPTOR = ROOT / "guest-linux/aarch64/common/obmm_vendor_adaptor_sim.c"
EXPECTED_PIN = "53011eed10716b422d2ac29199f68b55f7c5bdc5"

OBMM_COMMON_APPS = [
    "gsva_coh_test", "gsva_lifecycle_test", "gsva_query", "gva_direct",
    "gva_manager", "npu_gsva_test", "obmm_coh_test",
    "obmm_dataplane_microbench", "obmm_gsva", "obmm_import_stress",
    "ssd_gsva_test",
]


class ObmmSubmoduleContractTest(unittest.TestCase):
    def test_gitmodules_declares_obmm(self):
        text = GITMODULES.read_text()
        self.assertIn('[submodule "vendor/obmm"]', text)
        self.assertIn("url = https://atomgit.com/openeuler/obmm.git", text)
        self.assertIn("branch = master", text)

    def test_obmm_pinned_to_expected_revision(self):
        out = subprocess.run(
            ["git", "-C", str(ROOT), "ls-tree", "HEAD", "vendor/obmm"],
            check=True, capture_output=True, text=True,
        ).stdout
        self.assertIn(EXPECTED_PIN, out)

    def test_libobmm_mk_wires_submodule(self):
        text = LIBOBMM_MK.read_text()
        self.assertIn("vendor/obmm/src/libobmm/libobmm.c", text)
        self.assertIn("obmm_vendor_adaptor_sim.c", text)
        self.assertIn("kernel_ub/include/uapi", text)

    def test_sim_adaptor_implements_vendor_seam(self):
        text = SIM_ADAPTOR.read_text()
        for symbol in (
            "vendor_adapt_export", "free_vendor_info",
            "vendor_fixup_import_cmd", "vendor_cleanup_import_cmd",
            "vendor_fixup_preimport_cmd", "vendor_cleanup_preimport_cmd",
        ):
            self.assertIn(symbol, text)

    def test_obmm_apps_use_libobmm_mk(self):
        for app in OBMM_COMMON_APPS:
            with self.subTest(app=app):
                makefile = (
                    ROOT / "guest-linux/aarch64/apps" / app / "Makefile"
                ).read_text()
                self.assertIn("libobmm.mk", makefile)
                self.assertIn("$(OBMM_SRCS)", makefile)


if __name__ == "__main__":
    unittest.main()
```

**Step 2:** 运行验证失败：

```bash
cd /sd_data/repo/ub_sim && python3 -m unittest guest-linux/aarch64/tests/test_obmm_submodule_contract -v
```
预期：FAIL/ERROR（.gitmodules 无 vendor/obmm）。

### Task 2: 添加 vendor/obmm submodule

**Step 1:**
```bash
cd /sd_data/repo/ub_sim
git submodule add -b master https://atomgit.com/openeuler/obmm.git vendor/obmm
git -C vendor/obmm fetch origin && git -C vendor/obmm checkout 53011eed10716b422d2ac29199f68b55f7c5bdc5
```
**Step 2:** 编辑 `.gitmodules`，给该 entry 补 `ignore = all`（对齐既有条目格式）。
**Step 3:** `git submodule status vendor/obmm` → 前 7 位 `53011ee`。
**Step 4:** Task 1 测试中 submodule 相关 2 个用例转绿：
```bash
python3 -m unittest guest-linux/aarch64/tests/test_obmm_submodule_contract -v
```
预期：gitmodules/pin 两个 PASS，其余仍 FAIL（文件未建）。

### Task 3: sim 版 vendor adaptor + libobmm.mk

**Files:**
- Create: `guest-linux/aarch64/common/obmm_vendor_adaptor_sim.c`
- Create: `guest-linux/aarch64/common/libobmm.mk`

**Step 1:** adaptor（完整代码）：

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simulator vendor adaptor for libobmm.  Replaces the hardware-specific
 * vendor_adaptor.c from vendor/obmm at link time: the ub_sim guest has no
 * ub_bus_controller sysfs, and exports never carry EIDs.
 */

#include <errno.h>

#include <libobmm.h>
#include "vendor_adaptor.h"

int vendor_adapt_export(struct obmm_mem_desc *desc, const void **vendor_info,
            uint16_t *vendor_len, int *numa)
{
    (void)desc;
    *vendor_info = NULL;
    *vendor_len = 0;
    *numa = 0;
    return 0;
}

void free_vendor_info(void *vendor_info)
{
    (void)vendor_info;
}

int vendor_fixup_import_cmd(struct obmm_cmd_import *cmd)
{
    (void)cmd;
    return 0;
}

void vendor_cleanup_import_cmd(struct obmm_cmd_import *cmd)
{
    (void)cmd;
}

int vendor_fixup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
    (void)cmd;
    return 0;
}

void vendor_cleanup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
    (void)cmd;
}
```

注意：`vendor_adaptor.h` 用 `#include <libobmm.h>`（angle），编译需 `-I vendor/obmm/src/libobmm`；`obmm_cmd_preimport` 若 53011ee 的 UAPI 中不存在该结构体（先 `grep -n obmm_cmd_preimport vendor/obmm/src/libobmm/*.h guest-linux/kernel_ub/include/uapi/ub/obmm.h` 确认），缺哪个删哪个函数（header 声明了就必须实现，header 在 submodule 里不可改——若 header 声明了 kernel_ub 头没有的类型，实现保留并同样处理）。

**Step 2:** `guest-linux/aarch64/common/libobmm.mk`（完整代码）：

```make
# libobmm wiring shared by apps that include common/obmm_common.h.
# Include from an app Makefile as:  include ../common/libobmm.mk
OBMM_MK_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
AARCH64_ROOT := $(OBMM_MK_DIR)/..
UB_SIM_ROOT := $(AARCH64_ROOT)/../..
OBMM_SUBMODULE := $(UB_SIM_ROOT)/vendor/obmm
KERNEL_UB_UAPI := $(UB_SIM_ROOT)/guest-linux/kernel_ub/include/uapi
OBMM_CFLAGS := -I$(OBMM_SUBMODULE)/src/libobmm -I$(KERNEL_UB_UAPI)
OBMM_SRCS := $(OBMM_SUBMODULE)/src/libobmm/libobmm.c \
             $(OBMM_MK_DIR)/obmm_vendor_adaptor_sim.c
```

**Step 3:** 编译冒烟（host cc）：
```bash
cc -c -I guest-linux/aarch64/common -I vendor/obmm/src/libobmm \
   -I guest-linux/kernel_ub/include/uapi \
   guest-linux/aarch64/common/obmm_vendor_adaptor_sim.c -o /tmp/adaptor.o
```
预期：exit 0，无 warning。

**Step 4:** Task 1 测试 adaptor/mk 用例转绿。

### Task 4: golden 字节等价测试（先于改写，旧代码上跑通基线）

**Files:**
- Create: `guest-linux/aarch64/tests/obmm_libobmm_adapter_golden.c`
- Create: `guest-linux/aarch64/tests/test_obmm_libobmm_adapter_golden.py`

**Step 1:** C harness（完整代码，核心是 `--wrap=ioctl/open` 拦截 + golden 断言）：

```c
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Byte-equivalence harness for the obmm_common.h -> libobmm adapter layer.
 * Compiled with -Wl,--wrap=ioctl,--wrap=open so both the legacy raw-ioctl
 * path and the libobmm path are intercepted and compared against golden
 * obmm_cmd_* expectations.  Exits non-zero on any mismatch.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <libobmm.h>
#include "obmm_common.h"

static int g_failures;

#define CHECK(cond, name) do { \
    if (cond) { printf("ok %s\n", (name)); } \
    else { printf("FAIL %s\n", (name)); g_failures++; } \
} while (0)

extern int __real_ioctl(int, unsigned long, ...);
extern int __real_open(const char *, int, ...);

static struct obmm_cmd_export g_last_export;
static struct obmm_cmd_unexport g_last_unexport;
static struct obmm_cmd_import g_last_import;
static struct obmm_cmd_unimport g_last_unimport;
static int g_ioctl_count;

int __wrap_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/obmm") == 0)
        return 42;
    return __real_open(path, flags);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    void *arg;
    va_list ap;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    g_ioctl_count++;

    if (req == (unsigned long)OBMM_CMD_EXPORT) {
        struct obmm_cmd_export *cmd = arg;
        g_last_export = *cmd;
        /* kernel-side out params */
        cmd->mem_id = 0x1122334455667788ULL;
        cmd->uba = 0x2000000ULL;
        cmd->tokenid = 0x5a5a;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_UNEXPORT) {
        g_last_unexport = *(struct obmm_cmd_unexport *)arg;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_IMPORT) {
        struct obmm_cmd_import *cmd = arg;
        g_last_import = *cmd;
        cmd->mem_id = 0x99aabbccddeeff00ULL;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_UNIMPORT) {
        g_last_unimport = *(struct obmm_cmd_unimport *)arg;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

int main(void)
{
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_meta peer = {0};
    uint64_t import_mem_id = 0;
    int fd = obmm_open_device();
    int i;

    CHECK(fd == 42, "open(/dev/obmm)");

    /* ---- export ---- */
    meta.export_cna = 0;
    CHECK(obmm_do_export(fd, &meta, 0x100000) == 0, "do_export rc");
    CHECK(meta.export_mem_id == 0x1122334455667788ULL, "export mem_id");
    CHECK(meta.remote_uba == 0x2000000ULL, "export uba");
    CHECK(meta.token_id == 0x5a5a, "export tokenid");
    CHECK(meta.size == 0x100000, "export size");
    CHECK(g_last_export.size[0] == 0x100000, "cmd size[0]");
    {
        int rest_zero = 1;
        for (i = 1; i < OBMM_MAX_LOCAL_NUMA_NODES; i++)
            if (g_last_export.size[i] != 0) { rest_zero = 0; break; }
        CHECK(rest_zero, "cmd size[1..] zero");
    }
    CHECK(g_last_export.flags == OBMM_EXPORT_FLAG_ALLOW_MMAP, "cmd flags");
    CHECK(g_last_export.pxm_numa == 0, "cmd pxm_numa");
    CHECK(g_last_export.uba == 0, "cmd in uba zero");
    /* cmd.length intentionally differs: legacy 1, libobmm
       OBMM_MAX_LOCAL_NUMA_NODES (kernel skips zero entries). */
    CHECK(g_last_export.length == OBMM_MAX_LOCAL_NUMA_NODES,
          "cmd length (libobmm semantics)");

    /* ---- import v1 ---- */
    peer.remote_uba = 0x2000000ULL;
    peer.size = 0x100000;
    peer.token_id = 0x5a5a;
    peer.export_cna = 3;
    CHECK(obmm_do_import(fd, &peer, 7, 0x480000000ULL, 0xa1b2,
                         &import_mem_id) == 0, "do_import rc");
    CHECK(import_mem_id == 0x99aabbccddeeff00ULL, "import mem_id");
    CHECK(g_last_import.addr == 0x480000000ULL, "cmd addr");
    CHECK(g_last_import.length == 0x100000, "cmd length");
    CHECK(g_last_import.tokenid == 0x5a5a, "cmd tokenid");
    CHECK(g_last_import.scna == 7, "cmd scna");
    CHECK(g_last_import.dcna == 3, "cmd dcna");
    CHECK(g_last_import.flags == OBMM_IMPORT_FLAG_ALLOW_MMAP, "cmd flags");
    CHECK(g_last_import.base_dist == 0, "cmd base_dist");
    CHECK(g_last_import.numa_id == 0, "cmd numa_id");
    {
        const struct obmm_sim_dec_import_priv_v1 *priv =
            (const struct obmm_sim_dec_import_priv_v1 *)g_last_import.priv;
        CHECK(g_last_import.priv_len == sizeof(*priv), "cmd priv_len");
        CHECK(priv->magic == OBMM_SIM_DEC_PRIV_MAGIC, "priv magic");
        CHECK(priv->version == OBMM_SIM_DEC_PRIV_VER_1, "priv version");
        CHECK(priv->len == sizeof(*priv), "priv len");
        CHECK(priv->remote_uba == 0x2000000ULL, "priv remote_uba");
        CHECK(priv->token_value == 0xa1b2, "priv token_value");
    }

    /* ---- import v2 ---- */
    CHECK(obmm_do_import_v2_epoch(fd, &peer, 7, 0x480000000ULL, 0xa1b2,
            OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
            OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
            OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
            11, 22, 33, 44, OBMM_SIM_DEC_ACCESS_READ_ONLY,
            0x777, 9, 0x600000, 0x600000, 0x40, &import_mem_id) == 0,
          "do_import_v2 rc");
    {
        const struct obmm_sim_dec_import_priv_v2 *priv =
            (const struct obmm_sim_dec_import_priv_v2 *)g_last_import.priv;
        CHECK(g_last_import.priv_len == sizeof(*priv), "v2 priv_len");
        CHECK(priv->magic == OBMM_SIM_DEC_PRIV_MAGIC, "v2 magic");
        CHECK(priv->version == OBMM_SIM_DEC_PRIV_VER_2, "v2 version");
        CHECK(priv->remote_uba == 0x2000000ULL, "v2 remote_uba");
        CHECK(priv->token_value == 0xa1b2, "v2 token_value");
        CHECK(priv->map_source == OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
              "v2 map_source");
        CHECK(priv->address_profile ==
              OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY, "v2 profile");
        CHECK(priv->cache_policy ==
              OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI, "v2 cache_policy");
        CHECK(priv->vmid == 11, "v2 vmid");
        CHECK(priv->asid == 22, "v2 asid");
        CHECK(priv->tid == 33, "v2 tid");
        CHECK(priv->p_tag == 44, "v2 p_tag");
        CHECK(priv->access_flags == OBMM_SIM_DEC_ACCESS_READ_ONLY,
              "v2 access_flags");
        CHECK(priv->gva_id == 0x777, "v2 gva_id");
        CHECK(priv->segment_id == 0x777, "v2 segment_id");
        CHECK(priv->epoch == 9, "v2 epoch");
        CHECK(priv->local_va == 0x600000, "v2 local_va");
        CHECK(priv->home_va == 0x600000, "v2 home_va");
        CHECK(priv->pte_offset == 0x40, "v2 pte_offset");
    }

    /* ---- unexport / unimport ---- */
    CHECK(obmm_do_unexport(fd, 0x1122334455667788ULL) == 0, "unexport rc");
    CHECK(g_last_unexport.mem_id == 0x1122334455667788ULL, "unexport mem_id");
    CHECK(obmm_do_unimport(fd, 0x99aabbccddeeff00ULL) == 0, "unimport rc");
    CHECK(g_last_unimport.mem_id == 0x99aabbccddeeff00ULL, "unimport mem_id");

    if (g_failures == 0)
        printf("adapter-golden: ok (%d ioctls)\n", g_ioctl_count);
    else
        printf("adapter-golden: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

注意：harness 顶部需补 `#include <stdarg.h>`（`va_list`）；`g_last_import.priv` 是用户态指针字段（UAPI `__u64 priv`）——**若 `priv` 是 `__u64` 而非指针**，golden 断言前需 `(const void *)(uintptr_t)g_last_import.priv` 转换（执行时看 UAPI 定义调整，两处 priv 取法同理）。

**Step 2:** Python runner：

```python
import pathlib
import shutil
import subprocess
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
AARCH64 = TESTS_DIR.parent
UB_SIM = AARCH64.parent

HARNESS = TESTS_DIR / "obmm_libobmm_adapter_golden.c"
OBMM_SUBMODULE = UB_SIM / "vendor/obmm"
KERNEL_UB_UAPI = UB_SIM / "guest-linux/kernel_ub/include/uapi"
INCLUDES = [
    "-I", str(AARCH64 / "common"),
    "-I", str(OBMM_SUBMODULE / "src/libobmm"),
    "-I", str(KERNEL_UB_UAPI),
]
SOURCES = [
    str(HARNESS),
    str(OBMM_SUBMODULE / "src/libobmm/libobmm.c"),
    str(AARCH64 / "common/obmm_vendor_adaptor_sim.c"),
]


class ObmmLibobmmAdapterGoldenTest(unittest.TestCase):
    def test_adapter_matches_golden_ioctl_bytes(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("cc is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            binary = pathlib.Path(temp_dir) / "adapter_golden"
            subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", *INCLUDES, *SOURCES,
                 "-Wl,--wrap=ioctl", "-Wl,--wrap=open", "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run(
                [str(binary)], check=False, capture_output=True, text=True,
            )
            self.assertEqual(0, result.returncode, result.stdout)
            self.assertIn("adapter-golden: ok", result.stdout)
            self.assertNotIn("FAIL", result.stdout)


if __name__ == "__main__":
    unittest.main()
```

**Step 3:** 在**旧实现**上跑（基线校验 golden 期望本身正确）：
```bash
python3 -m unittest guest-linux/aarch64/tests/test_obmm_libobmm_adapter_golden -v
```
预期：PASS（此时 obmm_common.h 仍是 raw ioctl 路径；唯一例外 `cmd length` 断言——旧路径 length==1，此断言在旧代码上会 FAIL。处理：先把该断言写成条件式——检测当前路径：基线阶段将 length 断言临时按 1 校验并在 Task 5 改写后切换为 `OBMM_MAX_LOCAL_NUMA_NODES`？**不**——正确做法：这条断言直接期望 libobmm 语义，基线阶段允许这一条失败（记录 known-diff），Task 5 后必须全绿。执行时用环境变量 `GOLDEN_LEGACY=1` 切换该断言期望值，基线跑一次 legacy，改写后跑默认。）

**Step 4:** Commit（测试与 harness 属 ub_sim）：
```bash
git add guest-linux/aarch64/tests/obmm_libobmm_adapter_golden.c \
        guest-linux/aarch64/tests/test_obmm_libobmm_adapter_golden.py \
        guest-linux/aarch64/tests/test_obmm_submodule_contract.py \
        .gitmodules vendor/obmm \
        guest-linux/aarch64/common/obmm_vendor_adaptor_sim.c \
        guest-linux/aarch64/common/libobmm.mk
git commit -m "Add obmm submodule, sim vendor adaptor and golden adapter tests"
```

### Task 5: 验证内核 export 对零长度项的处理，然后改写 obmm_common.h

**Step 1:** 先验证（决定 export 走向）：
```bash
grep -n -A12 "size\[" guest-linux/kernel_ub/drivers/ub/obmm/obmm_export.c | head -40
```
确认循环跳过 `size[i] == 0`（真实内核同 UAPI，应当跳过）。**若不跳过**：export 保留 raw ioctl，只迁 import/unexport/unimport，并在 golden 中相应调整 length 断言回 1。

**Step 2:** 改写 `guest-linux/aarch64/common/obmm_common.h`（Edit，逐函数）：

5a. include 区（`#include "../../kernel_ub/..."` 两行之后）加：
```c
#include <libobmm.h>
```

5b. `obmm_do_export` 整函数替换：
```c
static int OBMM_MAYBE_UNUSED obmm_do_export(int obmm_fd, struct obmm_helpers_meta *meta,
                          uint64_t export_size)
{
    size_t length[OBMM_MAX_LOCAL_NUMA_NODES] = {0};
    struct obmm_mem_desc desc;
    mem_id id;

    (void)obmm_fd;
    memset(&desc, 0, sizeof(desc));
    length[0] = (size_t)export_size;
    id = obmm_export(length, OBMM_EXPORT_FLAG_ALLOW_MMAP, &desc);
    if (id == OBMM_INVALID_MEMID)
        return -1;
    meta->export_mem_id = (uint64_t)id;
    meta->remote_uba = desc.addr;
    meta->size = export_size;
    meta->token_id = desc.tokenid;
    return 0;
}
```

5c. `obmm_do_export_fixed_uba` **不动**（libobmm 无 requested_uba 输入路径）。

5d. `obmm_do_unexport` 替换：
```c
static int OBMM_MAYBE_UNUSED obmm_do_unexport(int obmm_fd, uint64_t mem_id)
{
    (void)obmm_fd;
    return obmm_unexport((mem_id)mem_id, 0);
}
```

5e. `obmm_do_import` 替换（priv v1 透传，numa 传 0 保持 `cmd.numa_id==0`）：
```c
static int OBMM_MAYBE_UNUSED obmm_do_import(int obmm_fd, const struct obmm_helpers_meta *meta,
                          uint32_t local_cna, uint64_t local_pa,
                          uint32_t token_value, uint64_t *import_mem_id)
{
    struct obmm_sim_dec_import_priv_v1 priv;
    struct obmm_mem_desc *desc;
    int numa = 0;
    mem_id id;

    (void)obmm_fd;
    memset(&priv, 0, sizeof(priv));
    priv.magic = OBMM_SIM_DEC_PRIV_MAGIC;
    priv.version = OBMM_SIM_DEC_PRIV_VER_1;
    priv.len = sizeof(priv);
    priv.remote_uba = meta->remote_uba;
    priv.token_value = token_value;

    desc = calloc(1, sizeof(*desc) + sizeof(priv));
    if (!desc)
        return -1;
    desc->addr = local_pa;
    desc->length = meta->size;
    desc->tokenid = meta->token_id;
    desc->scna = local_cna;
    desc->dcna = meta->export_cna;
    desc->priv_len = sizeof(priv);
    memcpy(desc->priv, &priv, sizeof(priv));

    id = obmm_import(desc, OBMM_IMPORT_FLAG_ALLOW_MMAP, 0, &numa);
    free(desc);
    if (id == OBMM_INVALID_MEMID)
        return -1;
    *import_mem_id = (uint64_t)id;
    return 0;
}
```
（`obmm_common.h` 已 include `<stdlib.h>`，calloc/free 可用。）

5f. `obmm_do_import_v2_epoch` 同型替换（priv 换 v2、字段按现有代码填入 `priv`，desc 堆分配 `sizeof(*desc) + sizeof(priv)`，flags/base_dist/numa 同上）；`obmm_do_import_v2`、`obmm_do_import_gsva_desc_v1` 包装层不动（它们只调 v2_epoch）。

5g. `obmm_do_unimport` 替换：
```c
static int OBMM_MAYBE_UNUSED obmm_do_unimport(int obmm_fd, uint64_t mem_id)
{
    (void)obmm_fd;
    return obmm_unimport((mem_id)mem_id, 0);
}
```

其余（bootstrap、GSVA mmap、窗口、网络助手、`obmm_open_device`）全部不动。

**Step 3:** golden 测试全绿（不再用 legacy 模式）：
```bash
python3 -m unittest guest-linux/aarch64/tests/test_obmm_libobmm_adapter_golden -v
```
预期：PASS，`adapter-golden: ok`。

**Step 4:** Commit：
```bash
git add guest-linux/aarch64/common/obmm_common.h
git commit -m "Route base obmm ops through libobmm adapter layer"
```

### Task 6: 11 个 app Makefile + build_initramfs.sh 接线

**Files（Modify，逐个 Edit）:**
- `guest-linux/aarch64/apps/{11 个 app}/Makefile`
- `guest-linux/aarch64/scripts/build_initramfs.sh`

**Step 1:** 典型 9 行 Makefile（以 obmm_gsva 为例）改后：
```make
CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..

include ../common/libobmm.mk

all: obmm_gsva

obmm_gsva: obmm_gsva.c
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. $(OBMM_CFLAGS) $(OBMM_SRCS) $< -o $@
```
（不同 app 的目标名/额外 flag 各异，只加三处：`include` 行、`$(OBMM_CFLAGS)`、`$(OBMM_SRCS)`。obmm_import_stress 等自带 CFLAGS 的照同样三处合并。）

**Step 2:** `build_initramfs.sh`：先 `grep -n "obmm_gsva\|obmm_coh_test\|..."` 找出全部内联编译行（11 个 app 名逐个 grep）；在脚本变量区（`INIT_SRC` 附近）加：
```bash
OBMM_SUBMODULE_DIR="$ROOT_DIR/../../vendor/obmm"
LIBOBMM_CFLAGS="-I$OBMM_SUBMODULE_DIR/src/libobmm -I$ROOT_DIR/../kernel_ub/include/uapi"
LIBOBMM_SRCS="$OBMM_SUBMODULE_DIR/src/libobmm/libobmm.c $ROOT_DIR/common/obmm_vendor_adaptor_sim.c"
```
（路径以脚本内既有相对基准为准，执行时核对 `$ROOT_DIR` 定义。）每条受影响的内联编译行追加 `$LIBOBMM_CFLAGS $LIBOBMM_SRCS`。

**Step 3:** 交叉编译冒烟（先一个）：
```bash
make -C guest-linux/aarch64/apps/obmm_gsva && make -C guest-linux/aarch64/apps/obmm_gsva clean
```
预期：exit 0。

**Step 4:** Task 1 契约测试全绿；Commit：
```bash
git add -A guest-linux/aarch64/apps guest-linux/aarch64/scripts/build_initramfs.sh
git commit -m "Wire obmm apps to libobmm via shared make fragment"
```

### Task 7: ub_sim 全量验证

**Step 1:** 构建矩阵（mem_service 条目此时仍用旧 pin，可先 `--from ub_chat` 跳过或全跑——旧 obmm_common 的 mem_service 拷贝不受影响，应照常绿）：
```bash
cd guest-linux/aarch64 && ./scripts/run_ub_app_build_matrix.sh
```
预期：全部 `RUN app=... ` 成功，exit 0。
**Step 2:** 全量 guest 测试：
```bash
python3 -m unittest discover guest-linux/aarch64/tests
```
预期：OK（0 failures）。
**Step 3:** 修复可能的编译告警/失败（单次少量修改），重复至全绿。此时不 commit（Task 8 一起）。

### Task 8: AGENTS.md 更新 + ub_sim 阶段收尾

**Step 1:** `AGENTS.md`：vendor 段落加一句 obmm submodule 说明；`git submodule update --init` 出现处补 `--recursive`（嵌套 mem_service/vendor/obmm）。
**Step 2:** Commit：
```bash
git add AGENTS.md
git commit -m "Document obmm submodule and recursive init"
```

### Task 9: mem_service 引入同名 submodule + 同步适配层

**工作目录：`/sd_data/repo/ub_sim/mem_service`（独立仓库）**

**Step 1:**
```bash
git -C mem_service status --short   # 必须干净
git -C mem_service submodule add -b master https://atomgit.com/openeuler/obmm.git vendor/obmm
git -C mem_service/vendor/obmm checkout 53011eed10716b422d2ac29199f68b55f7c5bdc5
```
`.gitmodules` 补 `ignore = all`。

**Step 2:** 同步 `common/obmm_common.h`：从 ub_sim 拷贝改后版本，再把两行 kernel_ub include 改回 mem_service 形态：
```c
#include "kernel_ub/include/uapi/ub/obmm.h"
#include "kernel_ub/include/uapi/ub/gsva.h"
```
新增的 `#include <libobmm.h>` 原样保留。拷贝 `obmm_vendor_adaptor_sim.c` 到 `mem_service/common/`（与 ub_sim 完全一致）。

**Step 3:** `apps/mem_service/Makefile`：
- 变量区加：
```make
OBMM_SUBMODULE := $(ROOT)/vendor/obmm
LIBOBMM_INCLUDES := -I$(OBMM_SUBMODULE)/src/libobmm -I$(ROOT)/kernel_ub/include/uapi
LIBOBMM_SRCS := $(OBMM_SUBMODULE)/src/libobmm/libobmm.c $(ROOT)/common/obmm_vendor_adaptor_sim.c
INCLUDES := -I$(ROOT) -I$(ROOT)/libs/obmm_queue $(LIBOBMM_INCLUDES)
```
- `$(MEM_SERVICE_CORE_SRCS)`（含 cluster_runtime 的链接组，line ~193 处的列表）追加 `$(LIBOBMM_SRCS)`；
- `linqu_mem_service_provider_obmm` 规则的依赖追加 `$(LIBOBMM_SRCS)`；
- grep Makefile 中所有链接 `mem_service_cluster_runtime.c` 或 `providers/mem_service_provider_obmm.c` 的目标（host 变体、conformance、smoke），逐个追加。
- `clean` 目标不变。

**Step 4:** pytest 编译入口：`grep -rn "mem_service_provider_obmm.c\|mem_service_cluster_runtime.c" mem_service/tests/*.py`，对每处 subprocess 编译命令加 `-I vendor/obmm/src/libobmm -I kernel_ub/include/uapi` 与两个源文件（`test_mem_service_obmm_provider.py` 的 `_compile`、conformance 编译、`test_repository_contract.py`、`test_mem_service_daemon_runtime.py`、`test_mem_service_record_recycling.py` 中涉及者）。

**Step 5:** VENDORED.md 与 `tests/test_repository_contract.py` 同步：
- 新源 revision = ub_sim Task 8 后的 commit SHA；
- `sha256sum mem_service/common/obmm_common.h` 更新两处（VENDORED.md + 测试硬编码）；
- obmm_queue 聚合与 kernel_ub 头不变（确认即可）；
- VENDORED.md 增补说明：libobmm 依赖经 `vendor/obmm` submodule（同 pin）。

**Step 6:** standalone 全量：
```bash
cd mem_service && python3 -m unittest discover tests
```
预期：OK。修复至全绿。

**Step 7:** mem_service commit（等用户 push 指示前先本地提交）：
```bash
git -C mem_service add -A && git -C mem_service commit -m "Route obmm provider through libobmm submodule"
```
记录新 HEAD SHA（gitlink bump 用）。

### Task 10: ⛔ 检查点——请求用户确认 push mem_service

向用户报告：mem_service 已 commit、测试全绿、新 SHA；等待 push 指令。**未 push 前不进行 Task 11**（AGENTS.md：gitlink/lock 仅在 remotely fetchable commit 后更新）。

### Task 11: bump ub_sim gitlink + mem_service.lock（push 后）

**Step 1:**
```bash
git -C mem_service push   # 仅在用户明确同意后
git -C /sd_data/repo/ub_sim add mem_service
```
**Step 2:** 更新 `guest-linux/aarch64/mem_service.lock`：`revision=<新 SHA>`；`version` 读 `mem_service/VERSION`（有变化才改）。
**Step 3:** 复验：
```bash
./guest-linux/aarch64/scripts/verify_mem_service_source.py \
  --mem-service-root mem_service --lock-file guest-linux/aarch64/mem_service.lock
cd guest-linux/aarch64 && ./scripts/run_ub_app_build_matrix.sh --only mem_service
python3 -m unittest discover guest-linux/aarch64/tests
```
预期：全绿。
**Step 4:**
```bash
git add mem_service guest-linux/aarch64/mem_service.lock
git commit -m "Bump mem_service to libobmm-based revision"
```

### Task 12: 交付报告

汇总：改动清单、测试结果原文、golden 等价证据、建议远程跑的 guest 脚本清单（`run_ub_dual_node_obmm_pool.sh`、`run_ub_dual_node_mem_service_obmm_provider_conformance.sh`、`run_ub_dual_node_obmm_gsva.sh`、`run_ub_eight_node_gsva_*` 等），由用户决定触发。

---

## 回归风险与预案

- **内核 export 零长度项**（Task 5 Step 1）：不跳过则 export 保留 raw ioctl。
- **`obmm_cmd_preimport` 不在 kernel_ub UAPI**：adaptor 对应函数签名若引用未知类型，改用 ub_sim 侧超集头编译（已 `-I`）；仍不行则实现为弱兼容（类型冲突时在 adaptor 内以最小重复定义解决，不改 submodule）。
- **UAPI 双重包含**：obmm_common.h 相对路径 include 与 libobmm.h 的 `<ub/obmm.h>` 指向同一文件（include guard 生效），无冲突。
- **mem_service 测试硬编码校验和**：漏改 `test_repository_contract.py` 会 FAIL，属预期检查点。
