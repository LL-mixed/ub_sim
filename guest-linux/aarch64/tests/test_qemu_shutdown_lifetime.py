"""Exercise production VM-stop entry points after migration teardown."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class VmShutdownLifetimeTests(unittest.TestCase):
    def test_builder_tracks_shutdown_source(self):
        builder = (ROOT / "guest-linux/aarch64/scripts/build_qemu_binary.sh").read_text()
        signature = builder.split("qemu_source_signature() {", 1)[1].split(
            "append_configure_arg_once()", 1
        )[0]
        self.assertIn('"$SRC_DIR/system/cpus.c"', signature)
        self.assertIn('"$SRC_DIR/tests/unit/test-gsva-route-lifetime.c"', signature)
        self.assertIn('"$SRC_DIR/tests/unit/test-gsva-retire.c"', signature)
        self.assertIn('"$SRC_DIR/tests/unit/meson.build"', signature)
        self.assertIn('-x "$BUILD_DIR/tests/unit/test-gsva-retire"', builder)
        self.assertIn('\n    tests/unit/test-gsva-retire\n', builder)

    def test_shutdown_and_normal_stop_lifetimes(self):
        compiler = (os.environ.get("CC") or
                    (shutil.which("gcc-15") if sys.platform.startswith("linux") else None) or
                    shutil.which("cc"))
        if not compiler:
            self.skipTest("requires C compiler with address/undefined sanitizers")
        source = (ROOT / "vendor/qemu_8.2.0_ub/system/cpus.c").read_text()
        stop = "static int do_vm_stop(" + source.split(
            "static int do_vm_stop(", 1
        )[1].split("/* Special vm_stop()", 1)[0]
        shutdown = "int vm_shutdown(void)" + source.split(
            "int vm_shutdown(void)", 1
        )[1].split("bool cpu_can_run(", 1)[0]
        normal = "int vm_stop(RunState state)" + source.split(
            "int vm_stop(RunState state)", 1
        )[1].split("/**", 1)[0]
        program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef enum { RUN_STATE_RUNNING, RUN_STATE_PAUSED, RUN_STATE_SHUTDOWN } RunState;
typedef struct MigrationState {
    int64_t downtime_start, notify_time, bdrv_time;
} MigrationState;
#define QEMU_CLOCK_REALTIME 0
#define qemu_log(...) ((void)0)
static MigrationState *current_migration;
static RunState current_state;
static int pause_count, notify_count, stop_count, drain_count, flush_count;
static int migration_lookups, request_count, cpu_stop_count;
static bool vcpu_thread;
static int64_t clock_ms;
static MigrationState *migrate_get_current(void)
{ migration_lookups++; return current_migration; }
static bool runstate_is_running(void)
{ return current_state == RUN_STATE_RUNNING; }
static void runstate_set(RunState state) { current_state = state; }
static void cpu_disable_ticks(void) {}
static int64_t qemu_clock_get_ms(int clock)
{ (void)clock; return ++clock_ms; }
static void pause_all_vcpus(void) { pause_count++; }
static void migration_downtime_start(MigrationState *s)
{ s->downtime_start = qemu_clock_get_ms(QEMU_CLOCK_REALTIME); }
static void trace_all_vcpus_paused(void) {}
static void vm_state_notify(int running, RunState state)
{ assert(!running && state == current_state); notify_count++; }
static void qapi_event_send_stop(void) { stop_count++; }
static void bdrv_drain_all(void) { drain_count++; }
static int bdrv_flush_all(void) { flush_count++; return -17; }
static void trace_vm_stop_flush_all(int ret) { assert(ret == -17); }
static bool qemu_in_vcpu_thread(void) { return vcpu_thread; }
static void qemu_system_vmstop_request_prepare(void) { request_count++; }
static void qemu_system_vmstop_request(RunState state)
{ assert(state == RUN_STATE_PAUSED); request_count++; }
static void cpu_stop_current(void) { cpu_stop_count++; }
''' + stop + shutdown + normal + r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    int mode = atoi(argv[1]);
    bool shutting_down = mode == 2 || mode == 3;
    bool was_running = mode == 0 || mode == 2;
    current_state = was_running ? RUN_STATE_RUNNING : RUN_STATE_PAUSED;
    current_migration = malloc(sizeof(*current_migration));
    assert(current_migration);
    *current_migration = (MigrationState){-1, -1, -1};
    vcpu_thread = mode == 4;
    if (shutting_down) {
        /* Match qemu_cleanup(): migration_shutdown precedes vm_shutdown. */
        free(current_migration);
    }
    int ret = shutting_down ? vm_shutdown() : vm_stop(RUN_STATE_PAUSED);
    if (vcpu_thread) {
        assert(ret == 0 && request_count == 2 && cpu_stop_count == 1);
        assert(!pause_count && !drain_count && !flush_count && !migration_lookups);
    } else {
        assert(ret == -17 && drain_count == 1 && flush_count == 1);
        assert(pause_count == was_running && notify_count == was_running);
        assert(stop_count == (was_running && !shutting_down));
        if (was_running) {
            assert(current_state == (shutting_down ? RUN_STATE_SHUTDOWN
                                                  : RUN_STATE_PAUSED));
        }
        if (shutting_down) {
            assert(!migration_lookups);
        } else {
            assert(current_migration->bdrv_time > 0);
            assert((current_migration->notify_time > 0) == was_running);
            assert((current_migration->downtime_start > 0) == was_running);
        }
    }
    if (!shutting_down) {
        free(current_migration);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(program)
            binary = path / "test"
            build = subprocess.run(
                [compiler, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 str(path / "test.c"), "-o", str(binary)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for mode in range(5):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), str(mode)],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn("runtime error", result.stderr)
