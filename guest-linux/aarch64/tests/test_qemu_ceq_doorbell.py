"""Exercise the QEMU CEQ consumer-doorbell recovery contract."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
QEMU_UBC = ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c"


class QemuCeqDoorbellTests(unittest.TestCase):
    def test_partial_consume_renotifies_nonempty_ceq(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")

        source = QEMU_UBC.read_text()
        function = source.split(
            "static bool ubc_handle_eq_doorbell(", 1
        )[1].split("\n}\n", 1)[0]
        program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define UBASE_EQ_DB_CMD_CEQ 0x2
#define UBASE_EQ_DB_EQN_MASK 0xFF
#define UBASE_EQ_DB_TYPE_SHIFT 16
#define UBASE_EQ_DB_TYPE_MASK 0x3
#define UBASE_EQ_DB_CI_SHIFT 32
#define UBASE_EQ_DB_INDEX_MASK 0xFFFFFF
#define UBC_MAX_CEQS 4
#define qemu_log(...) ((void)0)

typedef struct {
    bool active;
    uint32_t eq_depth;
    uint32_t eq_prod_index;
    uint32_t eq_ci;
    uint32_t irq_num;
} UBCEqState;
typedef struct { UBCEqState ceqs[UBC_MAX_CEQS]; } BusControllerDev;

static unsigned notifications;
static uint16_t notified_usi;
static uint16_t notified_msi;
static bool ubc_trace_data_path_enabled(void) { return false; }
static bool ubc_notify_vector(BusControllerDev *dev, uint16_t usi,
                              uint16_t msi, const char *name)
{
    (void)dev;
    (void)name;
    notifications++;
    notified_usi = usi;
    notified_msi = msi;
    return true;
}

static bool ubc_handle_eq_doorbell(""" + function + r"""
}

static uint64_t doorbell(uint32_t eqn, uint32_t type, uint32_t ci)
{
    return eqn | ((uint64_t)type << UBASE_EQ_DB_TYPE_SHIFT) |
           ((uint64_t)ci << UBASE_EQ_DB_CI_SHIFT);
}

int main(void)
{
    BusControllerDev dev = {0};
    UBCEqState *ceq = &dev.ceqs[1];

    assert(!ubc_handle_eq_doorbell(&dev, doorbell(1, 0, 1)));
    assert(notifications == 0);
    assert(ubc_handle_eq_doorbell(
        &dev, doorbell(UBC_MAX_CEQS, UBASE_EQ_DB_CMD_CEQ, 1)));
    assert(notifications == 0);
    assert(ubc_handle_eq_doorbell(
        &dev, doorbell(1, UBASE_EQ_DB_CMD_CEQ, 1)));
    assert(notifications == 0);

    ceq->active = true;
    ceq->eq_depth = 1024;
    ceq->irq_num = 9;
    ceq->eq_prod_index = 1023;
    assert(ubc_handle_eq_doorbell(
        &dev, doorbell(1, UBASE_EQ_DB_CMD_CEQ, 945)));
    assert(ceq->eq_ci == 945);
    assert(notifications == 1);
    assert(notified_usi == 9 && notified_msi == 3);

    assert(ubc_handle_eq_doorbell(
        &dev, doorbell(1, UBASE_EQ_DB_CMD_CEQ, 1023)));
    assert(ceq->eq_ci == 1023);
    assert(notifications == 1);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "ceq-doorbell"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                 "-o", str(binary)],
                input=program,
                text=True,
                capture_output=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=2
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_ceq_producer_and_ers1_write_use_doorbell_contract(self):
        source = QEMU_UBC.read_text()
        self.assertIn(
            "ceq->eq_prod_index = (ceq->eq_prod_index + 1)", source
        )
        self.assertIn(
            "ers->idx == 1 && addr == 0 && len == sizeof(uint64_t)", source
        )
        self.assertIn("ubc_handle_eq_doorbell(ubc_dev, val)", source)
        self.assertIn(
            "((ceq->eq_prod_index & 0xFFFFFFu) << 8)", source
        )


if __name__ == "__main__":
    unittest.main()
