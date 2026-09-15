"""Execute the actual kernel enumerator with a small in-memory segment table."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
KERNEL = ROOT / "guest-linux/kernel_ub"


class GsvaInventoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("requires C compiler")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "inventory"
        source = (KERNEL / "drivers/ub/obmm/obmm_core.c").read_text()
        header = (KERNEL / "include/uapi/ub/gsva.h").read_text()
        declarations = "\n".join(re.search(r"struct " + name + r" \{.*?\n\};", header, re.S)[0]
            for name in ("obmm_gsva_segment_desc_v1", "obmm_cmd_gsva_enumerate_v1"))
        definitions = "\n".join(line for line in header.splitlines()
            if line.startswith(("#define OBMM_GSVA_ENUM_", "#define OBMM_GSVA_RESOURCE_")))
        functions = re.search(r"static void obmm_gsva_changed\(void\).*?\n\}", source, re.S)[0]
        functions += "\n" + source[source.index("static int obmm_gsva_enumerate("):
                                   source.index("static int obmm_gsva_home_update(")]
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8, __u8;
typedef uint32_t u32, __u32;
typedef uint64_t u64, __u64;
#define OBMM_GSVA_ABI_VERSION 1
''' + definitions + "\n" + declarations + r'''
struct resource { void *parent; };
struct obmm_gsva_segment {
    struct obmm_gsva_segment_desc_v1 desc;
    struct resource address;
    u64 aperture_generation, managed_export_id;
    u32 managed_export_token;
    bool export_busy;
};
static struct obmm_gsva_segment segments[3];
static unsigned count, locks, random_calls;
static int obmm_gsva_segment_lock;
static u64 obmm_gsva_revision = 1, obmm_gsva_segment_counter;
static u8 obmm_gsva_kernel_instance[16];
#define list_for_each_entry(seg,head,node) for ((seg)=segments;(seg)<segments+count;(seg)++)
static void mutex_lock(int *lock) { (void)lock; assert(!locks++); }
static void mutex_unlock(int *lock) { (void)lock; assert(locks-- == 1); }
static void get_random_bytes(void *out, size_t len) { assert(locks); random_calls++; memset(out, 0x42, len); }
''' + functions + r'''
static void populate(void) {
    count = 3; obmm_gsva_segment_counter = 3;
    for (unsigned i=0;i<count;i++) {
        segments[i].desc=(struct obmm_gsva_segment_desc_v1){.version=1,
            .flags=i==1?11:7, .segment_id=(7ULL<<48)|(i+1),
            .home_va=0x700000000000ULL, .size=2097152, .epoch=i+1,
            .home_cna=7, .node_count=2, .token_id=10+i, .token_value=20+i};
        segments[i].aperture_generation=100+i;
        segments[i].address.parent=i==1?NULL:segments;
    }
    segments[0].managed_export_id=42; segments[0].managed_export_token=97;
    segments[2].export_busy=true;
    obmm_gsva_changed();
}
static void next(struct obmm_cmd_gsva_enumerate_v1 *cmd) {
    cmd->flags=0; assert(!obmm_gsva_enumerate(cmd) && !locks);
}
int main(int argc,char **argv) {
    assert(argc==2);
    _Static_assert(sizeof(struct obmm_cmd_gsva_enumerate_v1)==144,"ABI size");
    _Static_assert(offsetof(struct obmm_cmd_gsva_enumerate_v1,desc)==72,"ABI desc offset");
    struct obmm_cmd_gsva_enumerate_v1 cmd={.version=1}, old;
    if (!strcmp(argv[1],"empty")) {
        next(&cmd); assert(cmd.flags==OBMM_GSVA_ENUM_END && !cmd.cursor && cmd.revision==1);
        assert(cmd.kernel_instance[0] && random_calls==1);
        next(&cmd); assert(cmd.flags==OBMM_GSVA_ENUM_END && random_calls==1);
    } else if (!strcmp(argv[1],"complete")) {
        populate();
        for(unsigned i=0;i<count;i++) {
            next(&cmd); assert(cmd.flags==OBMM_GSVA_ENUM_ENTRY && cmd.cursor==i+1);
            assert(!memcmp(&cmd.desc,&segments[i].desc,sizeof(cmd.desc)));
            assert(cmd.aperture_generation==segments[i].aperture_generation);
            assert(cmd.resource_flags==(i==0?1:i==1?0:3));
            assert(cmd.export_mem_id==segments[i].managed_export_id);
            assert(cmd.export_token_id==segments[i].managed_export_token);
        }
        next(&cmd); assert(cmd.flags==OBMM_GSVA_ENUM_END && cmd.cursor==3);
        struct obmm_gsva_segment_desc_v1 zero={0};
        assert(!memcmp(&cmd.desc,&zero,sizeof(zero)) && !cmd.resource_flags);
        assert(!cmd.export_mem_id && !cmd.aperture_generation);
    } else if (!strcmp(argv[1],"stale")) {
        populate(); next(&cmd); cmd.flags=0; old=cmd;
        obmm_gsva_changed(); assert(obmm_gsva_enumerate(&cmd)==-ESTALE);
        assert(!memcmp(&cmd,&old,sizeof(cmd)));
        cmd.revision=obmm_gsva_revision; cmd.kernel_instance[15]^=1;
        assert(obmm_gsva_enumerate(&cmd)==-ESTALE);
        cmd=(struct obmm_cmd_gsva_enumerate_v1){.version=1};next(&cmd);
        assert(cmd.cursor==1 && cmd.revision==3);
    } else if (!strcmp(argv[1],"invalid")) {
        for(unsigned i=0;i<6;i++) {
            cmd=(struct obmm_cmd_gsva_enumerate_v1){.version=1};
            if(i==0)cmd.version=2;
            if(i==1)cmd.flags=1;
            if(i==2)cmd.reserved=1;
            if(i==3)cmd.reserved2=1;
            if(i==4)cmd.cursor=1;
            if(i==5)cmd.kernel_instance[0]=1;
            assert(obmm_gsva_enumerate(&cmd)==-EINVAL && !locks);
        }
        cmd=(struct obmm_cmd_gsva_enumerate_v1){.version=1};next(&cmd);
        cmd.flags=0;cmd.cursor=1;
        assert(obmm_gsva_enumerate(&cmd)==-EINVAL && !locks);
    } else {
        assert(!strcmp(argv[1],"overflow"));
        obmm_gsva_revision=UINT64_MAX;obmm_gsva_changed();
        assert(!obmm_gsva_revision && obmm_gsva_enumerate(&cmd)==-EOVERFLOW);
        obmm_gsva_changed();assert(!obmm_gsva_revision);
    }
    return 0;
}
'''
        result = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-x", "c", "-", "-o", str(cls.binary)], input=program,
            capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_empty_inventory_has_instance_and_explicit_end(self):
        self.run_case("empty")

    def test_all_active_retired_export_and_uncertain_records(self):
        self.run_case("complete")

    def test_mutation_or_other_kernel_rejects_whole_snapshot(self):
        self.run_case("stale")

    def test_invalid_request_and_cursor_rejected(self):
        self.run_case("invalid")

    def test_revision_overflow_permanently_invalidates_snapshots(self):
        self.run_case("overflow")


if __name__ == "__main__":
    unittest.main()
