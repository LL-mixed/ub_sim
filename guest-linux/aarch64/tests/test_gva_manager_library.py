import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST = ROOT / "guest-linux" / "aarch64"
LIB = GUEST / "libs" / "gva_manager"


def stage_uapi(directory):
    include = directory / "include"
    shutil.copytree(
        ROOT / "guest-linux" / "kernel_ub" / "include" / "uapi" / "ub",
        include / "ub", dirs_exist_ok=True,
    )
    return include


def compile_golden(compiler, output):
    include = stage_uapi(output.parent)
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         "-I", str(LIB), "-I", str(include),
         str(GUEST / "tests" / "gva_manager_golden.c"),
         str(LIB / "gva_manager.c"), "-o", str(output)],
        check=True,
    )


class GvaManagerLibraryTest(unittest.TestCase):
    @unittest.skipUnless(platform.system() == "Linux", "requires Linux UAPI")
    def test_kernel_allocation_capacity_precedes_reservation(self):
        source = (ROOT / "guest-linux/kernel_ub/drivers/ub/obmm/obmm_core.c").read_text()
        allocator = source.split("static int obmm_gsva_alloc_segment(", 1)[1].split(
            "\nstatic int obmm_gsva_query_segment", 1)[0]
        ioctl_case = source.split("case OBMM_CMD_GSVA_ALLOC_SEGMENT:", 1)[1].split(
            "case OBMM_CMD_GSVA_QUERY_SEGMENT:", 1)[0]
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ub/obmm.h>
#include <ub/gsva.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define PAGE_SIZE 4096
#define PAGE_ALIGNED(n) (!((n) & 4095))
#define PAGE_ALIGN(n) (((n) + 4095) & ~4095ULL)
#define GFP_KERNEL 0
#define IORESOURCE_MEM 1
#define IORESOURCE_BUSY 2
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
#define __user
#define check_add_overflow(a,b,p) __builtin_add_overflow(a,b,p)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct resource { u64 start, end; const char *name; unsigned flags; };
struct obmm_gsva_segment { struct resource address; struct obmm_gsva_segment_desc_v1 desc;
    u64 aperture_generation; int node; };
struct ub_entity { u32 cna; };
static struct ub_entity entity = {.cna=7};
static struct obmm_cmd_gsva_aperture obmm_gsva_aperture = {
    .base=0x700000000000ULL, .size=0x400000, .node_count=2,
    .flags=OBMM_GSVA_APERTURE_F_ACTIVE};
static struct resource obmm_gsva_address_space;
static u64 obmm_gsva_segment_counter = 31;
static unsigned revisions;
static void obmm_gsva_changed(void) { revisions++; }
static int obmm_gsva_aperture_lock, obmm_gsva_segment_lock, obmm_gsva_segments;
static int resource_error, registered, live, resource_calls;
static bool reserved, lose_output;
static struct obmm_gsva_segment *last_segment;
static void *kzalloc(size_t bytes, int flags) {
    (void)flags; assert(!live); last_segment=calloc(1,bytes);
    assert(last_segment); live++; return last_segment;
}
static void kfree(void *ptr) { assert(live==1 && !reserved); live--; free(ptr); }
static int ub_get_bus_controller(struct ub_entity **out, unsigned capacity, unsigned *count) {
    assert(capacity==1); *out=&entity; *count=1; return 0;
}
static int request_resource(struct resource *root, struct resource *entry) {
    (void)root; (void)entry; assert(!reserved); resource_calls++;
    if (!resource_error) reserved=true;
    return resource_error;
}
static int allocate_resource(struct resource *root, struct resource *entry,
    u64 size, u64 low, u64 high, u64 alignment, void *fn, void *data) {
    assert(!fn && !data && low==root->start && high==root->end);
    entry->start=(low+alignment-1)&~(alignment-1);
    entry->end=entry->start+size-1;
    return request_resource(root,entry);
}
static void list_add_tail(int *node, int *head) {
    (void)node; (void)head; assert(reserved && !registered); registered++;
}
static u32 obmm_gsva_next_token_id(void) { return 41; }
static u64 obmm_gsva_next_token_value(void) { return 73; }
static int copy_from_user(void *out, const void *in, size_t bytes) { memcpy(out,in,bytes); return 0; }
static int copy_to_user(void *out, const void *in, size_t bytes) {
    if (lose_output) return bytes;
    memcpy(out,in,bytes); return 0;
}
static int obmm_gsva_alloc_segment(''' + allocator + r'''
static int allocation_ioctl(struct obmm_cmd_gsva_alloc_segment_v1 *user) {
    int ret;
    union { struct obmm_cmd_gsva_alloc_segment_v1 gsva_alloc_seg; } cmd_param;
    unsigned long arg=(uintptr_t)user;
    switch (OBMM_CMD_GSVA_ALLOC_SEGMENT) {
    case OBMM_CMD_GSVA_ALLOC_SEGMENT:''' + ioctl_case + r'''
    default: assert(0);
    }
    return 0;
}
int main(void) {
    struct obmm_cmd_gsva_alloc_segment_v1 cmd={.version=1, .size=0x200000,
        .alignment=0x200000, .requested_p_tag=OBMM_GSVA_P_TAG_AUTO, .access_flags=3};
    const struct obmm_gsva_segment_desc_v1 empty={0};
    cmd.size=0x800000;
    assert(allocation_ioctl(&cmd)==-ENOSPC);
    assert(!live && !reserved && !registered && !resource_calls);
    assert(obmm_gsva_segment_counter==31 && !memcmp(&cmd.desc,&empty,sizeof(empty)));
    cmd.size=0x200000;
    resource_error=-EBUSY;
    assert(allocation_ioctl(&cmd)==-ENOSPC);
    assert(!live && !reserved && !registered && resource_calls==1);
    assert(obmm_gsva_segment_counter==31 && !memcmp(&cmd.desc,&empty,sizeof(empty)));
    resource_error=-ENOMEM;
    assert(allocation_ioctl(&cmd)==-ENOMEM);
    assert(!live && !reserved && !registered && obmm_gsva_segment_counter==31);
    resource_error=0; lose_output=true;
    assert(allocation_ioctl(&cmd)==-EFAULT);
    assert(live==1 && reserved && registered==1 && obmm_gsva_segment_counter==32);
    assert(revisions==1 && last_segment->aperture_generation==obmm_gsva_aperture.generation);
    assert(!memcmp(&cmd.desc,&empty,sizeof(empty))); /* Empty output alone proves nothing. */
    reserved=false; registered=0; kfree(last_segment);
    lose_output=false;
    assert(!allocation_ioctl(&cmd));
    assert(live==1 && reserved && registered==1 && obmm_gsva_segment_counter==33);
    assert(revisions==2);
    assert(cmd.desc.home_va==obmm_gsva_aperture.base && cmd.desc.segment_id);
    reserved=false; registered=0; kfree(last_segment);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "allocator-outcome"
            include = stage_uapi(binary.parent)
            compiled = subprocess.run(
                ["cc", "-Wall", "-Wextra", "-Werror", "-std=c11", "-I", str(include),
                 "-x", "c", "-", "-o", str(binary)], input=program,
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(platform.system() == "Linux", "native Linux CLI diagnostic test")
    def test_export_identity_cli_never_issues_a_valid_export(self):
        kernel = ROOT / "guest-linux" / "kernel_ub"
        vendor = ROOT / "vendor" / "obmm" / "src" / "libobmm"
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "export-identity-golden"
            include = stage_uapi(binary.parent)
            built = subprocess.run(
                ["cc", "-O2", "-Wall", "-Wextra", "-D__EXPORTED_HEADERS__", "-pthread",
                 "-I", str(include), "-I", str(GUEST / "common"),
                 "-I", str(GUEST / "libs" / "obmm_queue"), "-I", str(vendor),
                 "-idirafter", str(kernel / "include" / "uapi"),
                 "-idirafter", str(kernel / "include"),
                 str(GUEST / "tests/gva_manager_export_identity_golden.c"),
                 str(LIB / "gva_manager.c"), str(vendor / "libobmm.c"),
                 str(GUEST / "common/obmm_vendor_adaptor_sim.c"),
                 "-Wl,--wrap=ioctl", "-o", str(binary)],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("gva_manager_export_identity=pass checks=17 no_valid_export=1", result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_bootstrap_runner_rejects_invalid_config(self):
        runner = GUEST / "scripts" / "run_ub_dual_node_gsva_manager_bootstrap.sh"
        with tempfile.TemporaryDirectory() as directory:
            for args in ([str(pathlib.Path(directory) / "missing.env")], ["a", "b"]):
                with self.subTest(args=args):
                    result = subprocess.run(
                        ["zsh", str(runner), *args], capture_output=True,
                        text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertNotIn("starting nodeA", result.stdout)

    @unittest.skipUnless(platform.system() == "Linux", "requires Linux UAPI")
    def test_complete_inventory_cli_and_malformed_replies(self):
        kernel = ROOT / "guest-linux/kernel_ub"
        vendor = ROOT / "vendor/obmm/src/libobmm"
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "inventory-golden"
            include = stage_uapi(binary.parent)
            built = subprocess.run(
                ["cc", "-O2", "-Wall", "-Wextra", "-D__EXPORTED_HEADERS__", "-pthread",
                 "-I", str(include), "-I", str(GUEST / "common"),
                 "-I", str(GUEST / "libs/obmm_queue"), "-I", str(vendor),
                 "-idirafter", str(kernel / "include/uapi"),
                 "-idirafter", str(kernel / "include"),
                 str(GUEST / "tests/gva_manager_inventory_golden.c"),
                 str(LIB / "gva_manager.c"), str(vendor / "libobmm.c"),
                 str(GUEST / "common/obmm_vendor_adaptor_sim.c"),
                 "-Wl,--wrap=ioctl", "-o", str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("gva_manager_inventory=pass cases=20 no_partial_output=1", result.stdout)

    @unittest.skipUnless(platform.system() == "Linux", "requires Linux UAPI")
    def test_old_sdk_uapi_explicitly_rejects_inventory(self):
        sdk = ROOT / "mem_service/kernel_ub/include/uapi"
        program = '''
#include "gva_manager.h"
#include <assert.h>
#include <errno.h>
int main(void) {
    assert(gva_manager_enumerate_segments(-1, 0)==-1 && errno==EOPNOTSUPP);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "old-sdk-inventory"
            # Keep exercising the missing-ioctl build profile after the SDK
            # adopts enumeration. Header guards prevent its reintroduction.
            profile = pathlib.Path(directory) / "old-profile.h"
            profile.write_text('#include <ub/obmm.h>\n#undef OBMM_CMD_GSVA_ENUMERATE\n')
            built = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(LIB), "-I", str(sdk), "-include", str(profile), "-x", "c", "-",
                str(LIB / "gva_manager.c"), "-o", str(binary)], input=program,
                capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_creates_pid_directory_with_external_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            guest = pathlib.Path(directory) / "guest-linux" / "aarch64"
            scripts = guest / "scripts"
            scripts.mkdir(parents=True)
            runner = scripts / "run_ub_two_node_gva_manager_segment_cli_test.sh"
            shutil.copyfile(GUEST / "scripts" / runner.name, runner)
            # Exercise launch setup without starting QEMU or loading artifacts.
            (scripts / "qemu_ub_common.sh").write_text(
                'ensure_sim_kernel_append_defaults() { printf "%s" "$1"; }\n'
                'ensure_qemu_ub_binary() { printf "/usr/bin/true"; }\n'
                'ensure_ub_guest_artifacts() { :; }\n'
            )
            config = pathlib.Path(directory) / "test.env"
            config.write_text(
                f'UB_FM_SHARED_DIR="{directory}/links"\n'
                'LINK_WAIT_SECS=0\nRUN_ID=directory-test\n'
            )
            result = subprocess.run(
                ["zsh", str(runner), str(config)],
                capture_output=True, text=True, timeout=10,
            )
            self.assertTrue((guest / "out").is_dir())
            self.assertEqual(list((guest / "out").glob("*.pid")), [])
            self.assertEqual(result.returncode, 1)
            self.assertIn("FM links not ready", result.stderr)
            self.assertNotIn("no such file or directory", result.stderr)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_rejects_missing_config_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                ["zsh", str(GUEST / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh"),
                 str(pathlib.Path(directory) / "missing.env")],
                capture_output=True, text=True, timeout=10,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("config file not found", result.stderr)
        self.assertNotIn("starting nodeA", result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_rejects_extra_arguments_before_launch(self):
        result = subprocess.run(
            ["zsh", str(GUEST / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh"),
             "one.env", "two.env"], capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    @unittest.skipUnless(platform.system() == "Linux", "requires Linux UAPI")
    def test_kernel_results_and_failure_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "gva-manager-golden"
            compile_golden("cc", binary)
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True,
                timeout=10,
            )
        self.assertEqual(result.stdout.strip(), "gva_manager_golden=pass")

    def test_aarch64_compile(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler and platform.system() == "Linux" and platform.machine() == "aarch64":
            compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("aarch64 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            compile_golden(compiler, pathlib.Path(directory) / "gva-manager-golden")

    def test_existing_cli_links_library(self):
        native = platform.system() == "Linux"
        compiler = shutil.which("cc" if native else "aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("Linux compiler unavailable")
        kernel = ROOT / "guest-linux" / "kernel_ub"
        vendor = ROOT / "vendor" / "obmm" / "src" / "libobmm"
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "linqu_gva_manager"
            sysfs = pathlib.Path(directory) / "obmm-sysfs"
            include = stage_uapi(binary.parent)
            subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", "-D__EXPORTED_HEADERS__",
                 f'-DGVA_MGR_OBMM_SYSFS_ROOT="{sysfs}"',
                 "-pthread", "-I", str(include), "-I", str(GUEST / "common"),
                 "-I", str(GUEST / "libs" / "obmm_queue"), "-I", str(vendor),
                 "-idirafter", str(kernel / "include" / "uapi"),
                 "-idirafter", str(kernel / "include"),
                 str(GUEST / "apps" / "gva_manager" / "gva_manager.c"),
                 str(LIB / "gva_manager.c"), str(vendor / "libobmm.c"),
                 str(GUEST / "common" / "obmm_vendor_adaptor_sim.c"),
                 "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            if native:
                result = subprocess.run(
                    [str(binary), "--invalid-option"], capture_output=True, text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("gva_manager --alloc", result.stderr)
                self.assertNotIn("open /dev/obmm", result.stdout)
                self.check_region_census(binary, sysfs)

    def check_region_census(self, binary, sysfs):
        def run(code, *extra):
            result = subprocess.run(
                [str(binary), "--dump-obmm-regions", *extra],
                capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, code, result.stdout + result.stderr)
            if code:
                self.assertNotIn("obmm-kernel-region", result.stdout)
            return result.stdout

        run(1)  # Missing sysfs is unavailable, not a valid empty census.
        sysfs.mkdir()
        self.assertIn("export_handles=0 export_bytes=0 import_handles=0 import_bytes=0", run(0))
        run(2, "--alloc")
        regions = []
        for name, kind, size in (("obmm_shmdev1", "export", "0x200000"),
                                 ("obmm_shmdev2", "import", "0x400000")):
            region = sysfs / name
            region.mkdir()
            (region / "type").write_text(kind + "\n")
            (region / "size").write_text(size + "\n")
            regions.append(region)
        valid = run(0)
        self.assertIn("id=1 type=export bytes=2097152", valid)
        self.assertIn("id=2 type=import bytes=4194304", valid)
        self.assertIn("export_handles=1 export_bytes=2097152 import_handles=1 import_bytes=4194304", valid)
        for bad in ("0x0\n", "-1\n", "0x10000000000000000\n", "0x1junk\n", "0x1\nextra", "0x1"):
            with self.subTest(size=bad):
                (regions[1] / "size").write_text(bad)
                run(1)
        (regions[1] / "size").write_text("0x400000\n")
        (regions[1] / "type").write_text("unknown\n")
        run(1)
        (regions[1] / "type").write_text("export\n")
        (regions[0] / "size").write_text("0xffffffffffffffff\n")
        run(1)  # Sum overflow must not wrap into a plausible baseline.
        (regions[1] / "type").unlink()
        run(1)  # Disappearing/unreadable entry invalidates the whole observation.


if __name__ == "__main__":
    unittest.main()
