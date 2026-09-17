"""Compile the actual home registry; no VM, model, network or large allocation."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
QEMU = ROOT / "vendor/qemu_8.2.0_ub"


class GsvaHomeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("requires C compiler")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = pathlib.Path(cls.directory.name)
        (directory / "qemu").mkdir()
        (directory / "qemu/osdep.h").write_text(r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#define QEMU_PACKED __attribute__((packed))
#define g_try_new0(type, n) ((type *)calloc(n, sizeof(type)))
#define g_free free
""")
        (directory / "qemu/log.h").write_text("#define qemu_log(...) ((void)0)\n")
        cls.binary = directory / "gsva-home"
        program = r"""
#include "hw/ub/gsva_home.h"
_Static_assert(sizeof(GsvaHomeIdentity) == 88, "identity wire size");
_Static_assert(sizeof(GsvaHomeRequest) == 112, "registration wire size");
static GsvaHomeRequest original(void) {
    return (GsvaHomeRequest){.version = 1, .operation = GSVA_HOME_BIND,
        .identity = {.key = {.version = 1, .segment_id = 1,
            .home_va = 0x700000000000ULL, .size = 65536, .epoch = 1},
            .home_cna = 7, .token_id = 2, .token_value = 3,
            .backing_token_id = 97}, .export_mem_id = 2,
        .access_flags = GSVA_HOME_ACCESS_READ | GSVA_HOME_ACCESS_WRITE};
}
static int acquire(GsvaHomeTable *t, const GsvaHomeIdentity *i, bool write,
                   GsvaHomeBinding **pin) {
    return gsva_home_acquire(t, 7, i, i->key.home_va, 8, write, pin);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    GsvaHomeTable table = {0};
    GsvaHomeRequest old = original(), next = old, bad, resolved;
    GsvaHomeBinding *pin = NULL, *second = NULL;
    next.identity.key.segment_id = 4;
    next.identity.token_id = 5;
    next.identity.token_value = 6;
    assert(acquire(&table, &old.identity, false, &pin) != GSVA_OK && !pin);
    assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
    if (!strcmp(argv[1], "reuse")) {
        assert(acquire(&table, &old.identity, false, &pin) == GSVA_OK);
        old.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &old) == GSVA_ERR_COH_PENDING);
        assert(acquire(&table, &old.identity, false, &second) == GSVA_ERR_SEGMENT_RETIRED);
        assert(!second);
        assert(gsva_home_update(&table, 7, &next) == GSVA_ERR_COH_PENDING);
        gsva_home_release(pin);
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        assert(gsva_home_update(&table, 7, &next) == GSVA_OK);
        assert(acquire(&table, &old.identity, false, &pin) == GSVA_ERR_SEGMENT_RETIRED);
        assert(!pin);
        assert(acquire(&table, &next.identity, true, &pin) == GSVA_OK);
        gsva_home_release(pin);
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        old.operation = GSVA_HOME_BIND;
        assert(gsva_home_update(&table, 7, &old) == GSVA_ERR_SEGMENT_RETIRED);
        assert(acquire(&table, &next.identity, true, &pin) == GSVA_OK);
        gsva_home_release(pin);
    } else if (!strcmp(argv[1], "reset")) {
        assert(acquire(&table, &old.identity, false, &pin) == GSVA_OK);
        assert(gsva_home_reset(&table) == 1);
        assert(gsva_home_reset(&table) == 0);
        assert(acquire(&table, &old.identity, false, &second) == GSVA_ERR_SEGMENT_RETIRED);
        assert(!second);
        assert(gsva_home_update(&table, 7, &old) == GSVA_ERR_SEGMENT_RETIRED);
        assert(gsva_home_update(&table, 7, &next) == GSVA_ERR_COH_PENDING);
        old.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &old) == GSVA_ERR_COH_PENDING);
        assert(gsva_home_overlaps(&table, old.identity.key.home_va, 8));
        gsva_home_release(pin);
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        assert(gsva_home_update(&table, 7, &next) == GSVA_OK);
        assert(acquire(&table, &next.identity, true, &pin) == GSVA_OK);
        gsva_home_release(pin);
        assert(gsva_home_reset(&table) == 1);
        assert(acquire(&table, &next.identity, false, &pin) == GSVA_ERR_SEGMENT_RETIRED);
        assert(!pin);
    } else if (!strcmp(argv[1], "identity")) {
        /* Every packed identity byte is authoritative, including size/epoch. */
        for (size_t i = 0; i < sizeof(old.identity); ++i) {
            bad = old;
            ((uint8_t *)&bad.identity)[i] ^= 1;
            assert(acquire(&table, &bad.identity, false, &pin) != GSVA_OK);
            assert(!pin);
        }
        bad = old; bad.export_mem_id++;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_KEY_MISMATCH);
        bad.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_KEY_MISMATCH);
        bad = old; bad.identity.key.epoch++;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_KEY_MISMATCH);
        assert(acquire(&table, &old.identity, false, &pin) == GSVA_OK);
        gsva_home_release(pin);
    } else if (!strcmp(argv[1], "bounds")) {
        assert(gsva_home_acquire(&table, 8, &old.identity,
            old.identity.key.home_va, 8, false, &pin) == GSVA_ERR_TOKEN_DENIED);
        const uint64_t ranges[][2] = {{UINT64_MAX, 8}, {0, 0},
            {old.identity.key.home_va - 1, 8},
            {old.identity.key.home_va + 65535, 2}};
        for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
            assert(gsva_home_acquire(&table, 7, &old.identity,
                ranges[i][0], ranges[i][1], false, &pin) == GSVA_ERR_STRICT_ADDRESS);
            assert(!pin);
        }
        assert(gsva_home_acquire(&table, 7, &old.identity,
            old.identity.key.home_va + 65535, 1, false, &pin) == GSVA_OK);
        gsva_home_release(pin);
        assert(gsva_home_overlaps(&table, old.identity.key.home_va - 1, 2));
        assert(!gsva_home_overlaps(&table, old.identity.key.home_va - 8, 8));
        assert(gsva_home_overlaps(&table, UINT64_MAX, 8));
        old.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        assert(gsva_home_overlaps(&table, old.identity.key.home_va, 8));
    } else if (!strcmp(argv[1], "registration")) {
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        assert(gsva_home_update(&table, 8, &old) == GSVA_ERR_TOKEN_DENIED);
        assert(gsva_home_update(&table, 7, &next) == GSVA_ERR_COH_PENDING);
        next.identity.key.home_va += 131072;
        next.access_flags = GSVA_HOME_ACCESS_READ;
        assert(gsva_home_update(&table, 7, &next) == GSVA_OK);
        assert(acquire(&table, &next.identity, true, &pin) == GSVA_ERR_TOKEN_DENIED);
        assert(acquire(&table, &next.identity, false, &pin) == GSVA_OK);
        gsva_home_release(pin);
        bad = next; bad.access_flags = GSVA_HOME_ACCESS_READ | GSVA_HOME_ACCESS_WRITE;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_KEY_MISMATCH);
        bad = next; bad.identity.key.segment_id++; bad.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_ROUTE_MISSING);
        bad = next; bad.reserved = 1;
        assert(gsva_home_update(&table, 7, &bad) == GSVA_ERR_BAD_VERSION);
    } else if (!strcmp(argv[1], "export")) {
        memset(&resolved, 0xa5, sizeof(resolved));
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id,
            old.identity.backing_token_id, old.identity.key.home_va,
            old.identity.key.size, &resolved) == GSVA_OK);
        assert(!memcmp(&resolved, &old, sizeof(old)));
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id,
            old.identity.backing_token_id + 1, old.identity.key.home_va,
            old.identity.key.size, &resolved) == GSVA_ERR_TOKEN_DENIED);
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id + 1,
            old.identity.backing_token_id, old.identity.key.home_va,
            old.identity.key.size, &resolved) == GSVA_ERR_KEY_MISMATCH);
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id,
            old.identity.backing_token_id, old.identity.key.home_va + 1,
            old.identity.key.size, &resolved) == GSVA_ERR_KEY_MISMATCH);
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id + 1,
            old.identity.backing_token_id,
            old.identity.key.home_va + old.identity.key.size, 8,
            &resolved) == GSVA_ERR_ROUTE_MISSING);
        old.operation = GSVA_HOME_REVOKE;
        assert(gsva_home_update(&table, 7, &old) == GSVA_OK);
        assert(gsva_home_resolve_export(&table, 7, old.export_mem_id,
            old.identity.backing_token_id, old.identity.key.home_va,
            old.identity.key.size,
            &resolved) == GSVA_ERR_SEGMENT_RETIRED);
    } else {
        assert(!"unknown test case");
    }
    gsva_home_destroy(&table);
    assert(!table.bindings);
    return 0;
}
"""
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(directory), "-I", str(QEMU), "-x", "c", "-",
             str(QEMU / "hw/ub/gsva_home.c"), str(QEMU / "hw/ub/gsva_key.c"),
             "-o", str(cls.binary)], input=program, text=True,
            capture_output=True, timeout=15)
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_same_backing_token_reuse_and_inflight_revoke(self):
        self.run_case("reuse")

    def test_every_identity_byte_and_export_lifetime(self):
        self.run_case("identity")

    def test_bounds_wrong_home_and_legacy_tombstone(self):
        self.run_case("bounds")

    def test_registration_conflicts_and_read_only(self):
        self.run_case("registration")

    def test_exact_managed_export_resolution(self):
        self.run_case("export")
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        bootstrap = source.split(
            "static int sim_dec_register_obmm_gsva_route(", 1
        )[1].split("/* Helper for sim_dec_gva_tcg_translate", 1)[0]
        self.assertIn("gsva_home_resolve_export(", bootstrap)
        self.assertIn("key = managed.identity.key;", bootstrap)
        self.assertIn("token_id = managed.identity.token_id;", bootstrap)
        self.assertIn("route->backing_token_id = record->token_id;", bootstrap)
        self.assertIn("existing->cpu_window_initialized", bootstrap)
        self.assertIn("existing->cpu_window_mapped", bootstrap)
        self.assertIn("gsva_route_init_cpu_window(", bootstrap)
        self.assertIn("route->cpu_window_mapped = true;", bootstrap)
        self.assertIn("managed bootstrap identity rejected", bootstrap)
        publisher = source.split(
            "static int sim_dec_handle_obmm_bootstrap_publish(", 1
        )[1].split("static int sim_dec_load_bootstrap_record", 1)[0]
        self.assertIn("route_status = sim_dec_register_obmm_gsva_route(record)",
                      publisher)
        self.assertIn("return route_status;", publisher)

    def test_home_revoke_finalizes_the_exact_managed_route(self):
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        handler = source.split("case SIM_DEC_OP_GSVA_HOME_V1:", 1)[1].split(
            "case SIM_DEC_OP_MAP:", 1
        )[0]
        self.assertIn("request->operation == GSVA_HOME_REVOKE", handler)
        self.assertIn("ubc_gsva_complete_home_retire(", handler)
        self.assertIn("route_error != GSVA_ERR_ROUTE_MISSING", handler)
        unmap = source.split("static int sim_dec_handle_gsva_unmap(", 1)[1].split(
            "int ubc_gsva_complete_home_retire(", 1
        )[0]
        self.assertIn("route->backing_token_id ?", unmap)
        self.assertIn("route->backing_token_id : route->token.token_id", unmap)

    def test_local_strict_io_bypasses_and_restores_the_cpu_window(self):
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        body = source.split(
            "static MemTxResult ubc_sim_dec_remote_io_body(" , 2
        )[2].split(
            "MemTxResult ubc_sim_dec_remote_read(", 1
        )[0]
        local = body.split("if (dcna == ubc_dev->parent.cna) {", 1)[1].split(
            "link = ubc_find_active_link", 1
        )[0]
        self.assertIn("gsva_home_acquire(", local)
        self.assertIn("gsva_route_lookup_base(&g_gsva_routes", local)
        self.assertIn("ubc_cpu_window_detach(&home_route->cpu_window)", local)
        self.assertIn("ubc_dma_write_local_data_tid_strict(", local)
        self.assertIn("memory_region_add_subregion_overlap(", local)
        self.assertLess(local.index("ubc_cpu_window_detach("),
                        local.index("ubc_dma_write_local_data_tid_strict("))
        self.assertLess(local.index("ubc_dma_read_local_data_tid_strict("),
                        local.index("memory_region_add_subregion_overlap("))

    def test_routine_cpu_window_hits_are_page_scoped(self):
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        translate = source.split(
            "int gsva_arm_mmu_translate_full(", 1
        )[1].split(
            "int gsva_arm_mmu_translate(", 1
        )[0]
        self.assertIn("gsva_tlb_stable_matches(page_va", translate)
        self.assertIn("gsva_tlb_stable_set(page_va", translate)
        self.assertIn("if (!tlb_hit || ubc_trace_data_path_enabled())", translate)

        coherence = (QEMU / "hw/ub/gsva_coherence.c").read_text()
        read_acquire = coherence.split(
            "int gsva_coh_read_acquire_tx(", 1
        )[1].split(
            "int gsva_coh_write_acquire_tx(", 1
        )[0]
        write_acquire = coherence.split(
            "int gsva_coh_write_acquire_tx(", 1
        )[1].split(
            "int gsva_coh_retire(", 1
        )[0]
        self.assertIn("bool existing_sharer", read_acquire)
        self.assertIn(
            "if (!existing_sharer || gsva_coh_trace_hits_enabled())",
            read_acquire,
        )
        self.assertIn("if (gsva_coh_trace_hits_enabled())", write_acquire)

    def test_legacy_export_retire_removes_route_before_export_record(self):
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        helper = source.split(
            "static int sim_dec_retire_obmm_export_route("
            "const ObmmExportEntry *entry)\n{", 1
        )[1].split(
            "/* Helper for sim_dec_gva_tcg_translate", 1
        )[0]
        self.assertIn("SIM_DEC_MAP_SOURCE_LEGACY_OBMM", helper)
        self.assertIn("route->backing_token_id != 0", helper)
        self.assertIn("route->state = GSVA_ROUTE_STALE;", helper)
        self.assertIn("obmm_coh_send_fence(", helper)
        self.assertIn("gsva_coh_object_remove(&g_gsva_coh, &route->key)",
                      helper)
        self.assertIn("gsva_route_unmap(&g_gsva_routes, map_id, false)",
                      helper)
        handler = source.split(
            "static int sim_dec_handle_obmm_export_retire(\n", 1
        )[1].split(
            "static int sim_dec_handle_obmm_bootstrap_publish(", 1
        )[0]
        cleanup = handler.index("sim_dec_retire_obmm_export_route(entry)")
        marker = handler.index("g_file_set_contents(tmp_path, data")
        removal = handler.index("QTAILQ_REMOVE(&g_obmm_exports, entry, next)")
        self.assertLess(cleanup, marker)
        self.assertLess(marker, removal)

    def test_device_reset_closes_bindings_preserves_pins_and_denies_reactivation(self):
        self.run_case("reset")
        source = (QEMU / "hw/ub/ub_ubc.c").read_text()
        reset = source.split("static void ub_bus_controller_dev_reset(", 1)[1].split(
            "\n}\n", 1)[0]
        self.assertLess(reset.index("gsva_home_reset(&ubc_dev->gsva_home)"),
                        reset.index("ubc_void_pending_responses_cleanup"))
        self.assertNotIn("gsva_home_destroy", reset)

    def test_kernel_export_publication_and_revoke_failures(self):
        kernel = ROOT / "guest-linux/kernel_ub"
        source = (kernel / "drivers/ub/obmm/obmm_core.c").read_text()
        uapi = (kernel / "include/uapi/ub/gsva.h").read_text()
        declarations = "\n".join(re.search(
            r"struct " + name + r" \{.*?\n\};", uapi, re.S)[0]
            for name in ("obmm_gsva_segment_desc_v1", "obmm_cmd_gsva_export_segment_v1"))
        functions = "static int obmm_gsva_export_segment(" + source.split(
            "static int obmm_gsva_export_segment(", 1)[1].split(
            "static int obmm_gsva_retire_segment(", 1)[0]
        program = r"""
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32, __u32;
typedef uint64_t u64, __u64;
typedef int32_t __s32;
#define OBMM_GSVA_ABI_VERSION 1
#define OBMM_GSVA_EXPORT_F_FAST 1
#define OBMM_GSVA_EXPORT_UNKNOWN 0
#define OBMM_GSVA_EXPORT_CREATED 1
#define OBMM_GSVA_EXPORT_NO_BACKING 2
#define OBMM_GSVA_SEG_F_ACTIVE 4
#define OBMM_GSVA_SEG_F_RETIRED 8
#define OBMM_EXPORT_FLAG_ALLOW_MMAP 1
#define OBMM_EXPORT_FLAG_GSVA_FIXED_UBA 2
#define OBMM_EXPORT_FLAG_FAST 4
#define SIM_DEC_GSVA_HOME_BIND 1
#define SIM_DEC_GSVA_HOME_REVOKE 2
""" + declarations + r"""
struct obmm_gsva_segment {
    struct obmm_gsva_segment_desc_v1 desc;
    bool export_busy;
    u64 managed_export_id;
    u32 managed_export_token;
};
struct obmm_cmd_export { u64 length, size[1], uba, flags, mem_id; u32 tokenid; };
static struct obmm_gsva_segment segment;
static int obmm_gsva_segment_lock, pool_error, home_error, route_error;
static bool locked, unbacked;
static unsigned calls, route_calls;
static unsigned revisions;
static void obmm_gsva_changed(void) { assert(locked); revisions++; }
static void mutex_lock(int *lock) { (void)lock; assert(!locked); locked = true; }
static void mutex_unlock(int *lock) { (void)lock; assert(locked); locked = false; }
static struct obmm_gsva_segment *obmm_gsva_find_segment_by_id(u64 id)
{ assert(locked); return id == segment.desc.segment_id ? &segment : NULL; }
static struct obmm_gsva_segment *obmm_gsva_find_segment_by_va(u64 address)
{ assert(locked); return address == segment.desc.home_va ? &segment : NULL; }
static int obmm_export_from_pool_checked(struct obmm_cmd_export *request, bool *no_backing)
{
    assert(!locked && segment.export_busy);
    assert(request->size[0] == segment.desc.size && request->uba == segment.desc.home_va);
    *no_backing = unbacked;
    if (!pool_error) { request->mem_id = 2; request->tokenid = 97; }
    return pool_error;
}
static int obmm_gsva_home_update(const struct obmm_gsva_segment_desc_v1 *desc,
                                u64 mem_id, u32 token, u32 operation)
{
    assert(!locked && segment.export_busy && segment.managed_export_id == 2);
    assert(!memcmp(desc, &segment.desc, sizeof(*desc)) && mem_id == 2 && token == 97);
    assert(operation == 1 || operation == 2);
    calls++;
    return home_error;
}
static int obmm_gsva_publish_route(const struct obmm_gsva_segment_desc_v1 *desc,
                                  u64 mem_id, u32 token)
{
    assert(!locked && segment.export_busy && segment.managed_export_id == 2);
    assert(!memcmp(desc, &segment.desc, sizeof(*desc)) && mem_id == 2 && token == 97);
    route_calls++;
    return route_error;
}
""" + functions + r"""
int main(void) {
    for (int failure = 0; failure < 4; ++failure) {
        segment = (struct obmm_gsva_segment){.desc = {.version = 1,
            .flags = OBMM_GSVA_SEG_F_ACTIVE, .segment_id = 1,
            .home_va = 0x700000000000ULL, .size = 2097152}};
        struct obmm_cmd_gsva_export_segment_v1 cmd = {.version = 1, .segment = segment.desc};
        calls = route_calls = revisions = 0;
        home_error = failure == 1 ? -EIO : 0;
        route_error = failure == 2 ? -EIO : 0;
        pool_error = failure == 3 ? -ENOMEM : 0; unbacked = failure == 3;
        assert(obmm_gsva_export_segment(&cmd) == 0);
        assert(!locked && !segment.export_busy);
        assert(revisions == (failure == 3 ? 2U : 3U));
        if (failure == 3) {
            assert(cmd.outcome == OBMM_GSVA_EXPORT_NO_BACKING && !calls && !route_calls);
            assert(!segment.managed_export_id && !cmd.export_mem_id);
            continue;
        }
        assert(calls == 1 && segment.managed_export_id == 2);
        assert(route_calls == (failure == 1 ? 0U : 1U));
        assert(segment.managed_export_token == 97 && cmd.export_mem_id == 2);
        assert(cmd.outcome == (failure ? OBMM_GSVA_EXPORT_UNKNOWN : OBMM_GSVA_EXPORT_CREATED));
        assert(cmd.error == (failure ? EIO : 0));
        assert(obmm_gsva_export_segment(&cmd) == -EBUSY);
        home_error = -EBUSY;
        assert(obmm_gsva_export_revoke(segment.desc.home_va, 2) == -EBUSY);
        assert(segment.managed_export_id == 2 && !segment.export_busy);
        assert(obmm_gsva_export_revoke(segment.desc.home_va, 3) == -ESTALE);
        home_error = 0;
        assert(obmm_gsva_export_revoke(segment.desc.home_va, 2) == 0);
        assert(segment.managed_export_id == 2);
        obmm_gsva_export_released(segment.desc.home_va, 3);
        assert(segment.managed_export_id == 2);
        obmm_gsva_export_released(segment.desc.home_va, 2);
        assert(!segment.managed_export_id && !segment.managed_export_token);
        assert(revisions == 8);
        segment.export_busy = true;
        assert(obmm_gsva_export_revoke(segment.desc.home_va, 2) == -EBUSY);
    }
    return 0;
}
"""
        binary = pathlib.Path(self.directory.name) / "kernel-export"
        result = subprocess.run(
            [shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-x", "c", "-", "-o", str(binary)], input=program, text=True,
            capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        route = source.split("static int obmm_gsva_publish_route(", 1)[1].split(
            "static int obmm_gsva_export_segment(", 1
        )[0]
        self.assertIn(".generation = desc->segment_id", route)
        self.assertIn(".node_id = desc->owner_node_id", route)
        self.assertIn("record.backing_uba = sg_phys(export->sgt.sgl);", route)
        self.assertIn("export->sim_bootstrap_published = true;", route)


if __name__ == "__main__":
    unittest.main()
