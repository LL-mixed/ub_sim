"""Execute the production RX loop envelope without starting QEMU."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class UbLinkRxBudgetTests(unittest.TestCase):
    def test_send_queue_counters_wrap_without_replaying_old_entries(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        function = source.split("static void ubc_process_sq(", 1)[1].split("\n}\n", 1)[0]
        program = r"""
#include <stdint.h>
#include <assert.h>
typedef int BusControllerDev;
typedef struct { uint32_t sq_pi, sq_ci, sq_depth, jetty_id; } UBCJettyState;
#define qemu_log(...) ((void)0)
static unsigned dispatched;
static uint32_t expected;
static void ubc_process_sq_wqe(BusControllerDev *dev, UBCJettyState *js, uint32_t slot)
{
    (void)dev;
    assert(slot == expected % js->sq_depth);
    expected++;
    dispatched++;
    assert(dispatched < 1000);
}
static void ubc_process_sq(""" + function + r"""
}
int main(void)
{
    BusControllerDev dev = 0;
    UBCJettyState queue = {.sq_depth = 8};
    for (unsigned iteration = 0; iteration < 32; iteration++) {
        queue.sq_pi += 8;
        ubc_process_sq(&dev, &queue);
        assert(queue.sq_ci == queue.sq_pi);
    }
    assert(dispatched == 256);
    ubc_process_sq(&dev, &queue);
    assert(dispatched == 256);
    queue.sq_ci = UINT32_MAX - 3;
    queue.sq_pi = 4;
    expected = queue.sq_ci;
    ubc_process_sq(&dev, &queue);
    assert(queue.sq_ci == 4 && dispatched == 264);
    queue.sq_pi = 13; /* More than one ring outstanding. */
    ubc_process_sq(&dev, &queue);
    assert(queue.sq_ci == 4 && dispatched == 264);
    queue.sq_pi = 3; /* Stale/backwards doorbell. */
    ubc_process_sq(&dev, &queue);
    assert(queue.sq_ci == 4 && dispatched == 264);
    queue.sq_depth = 0;
    ubc_process_sq(&dev, &queue);
    assert(queue.sq_ci == 4 && dispatched == 264);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sq-wrap"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(binary)],
                input=program, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_continuously_ready_peer_yields_and_empty_peer_returns(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/hisi/ubc_msgq.c").read_text()
        envelope = source.split("void ub_link_process_incoming_message(", 1)[1]
        envelope = envelope.split("        MsgPktHeader *header", 1)[0]
        program = r"""
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
typedef int BusControllerState;
typedef int UBLinkState;
typedef int Error;
static unsigned attempts, handled;
static int outcome = 1;
#define qemu_log(...) ((void)0)
static void error_free(Error *error) { (void)error; }
static int ub_link_read_message(UBLinkState *link, void **buf, size_t *len,
                                Error **error)
{
    static int frame;
    (void)link; (void)error;
    attempts++;
    *buf = &frame;
    *len = sizeof(frame);
    return outcome;
}
void ub_link_process_incoming_message(""" + envelope + r"""
        /* Substitute only dispatch; execute the real production loop. */
        handled++;
    }
}
int main(void)
{
    BusControllerState controller = 0;
    UBLinkState link = 0;
    ub_link_process_incoming_message(&controller, &link);
    assert(attempts == 64 && handled == 64);
    ub_link_process_incoming_message(&controller, &link);
    assert(attempts == 128 && handled == 128);
    outcome = 0;
    ub_link_process_incoming_message(&controller, &link);
    assert(attempts == 129 && handled == 128);
    outcome = -1;
    ub_link_process_incoming_message(&controller, &link);
    assert(attempts == 130 && handled == 128);
    ub_link_process_incoming_message(NULL, &link);
    ub_link_process_incoming_message(&controller, NULL);
    assert(attempts == 130);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "rx-budget"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(binary)],
                input=program, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0, result.stderr)
