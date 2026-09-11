"""Lightweight layout checks compile actual kernel/QEMU declarations, no guest."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
KERNEL = ROOT / "guest-linux/kernel_ub/drivers/ub/ubus/sim"
QEMU = ROOT / "vendor/qemu_8.2.0_ub/hw/ub"


class GsvaImportWireTests(unittest.TestCase):
    def test_cpu_window_returns_faults_and_uses_export_token(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (QEMU / "ub_ubc.c").read_text()
        functions = "static MemTxResult sim_dec_gsva_cpu_window_read" + source.split(
            "static MemTxResult sim_dec_gsva_cpu_window_read", 1)[1].split(
            "\nstatic const MemoryRegionOps sim_dec_gsva_cpu_window_ops", 1)[0]
        program = r"""
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
typedef uint64_t hwaddr;
typedef int MemTxResult;
typedef int MemTxAttrs;
#define MEMTX_OK 0
#define MEMTX_DECODE_ERROR 1
#define MEMTX_ACCESS_ERROR 2
#define GSVA_OK 0
#define qemu_log(...) ((void)0)
typedef struct {
  struct { uint64_t size; } key;
  struct { uint32_t token_id, token_value; } token;
  uint32_t home_cna, backing_token_id;
  uint64_t map_id, remote_uba;
} GsvaRouteEntry;
static struct { struct { void *ubc_dev; } *bcs; } decoder, *g_sim_decoder = &decoder;
static int token_error, io_error;
static unsigned reads, writes;
static int gsva_route_validate_token(const GsvaRouteEntry *route, uint32_t cna,
                                    uint32_t id, uint32_t value, uint32_t access)
{ (void)route; assert(cna == 7 && id == 2 && value == 3 && (access == 1 || access == 2));
  return token_error; }
static MemTxResult ubc_sim_dec_remote_io(void *dev, uint64_t address, uint32_t token,
    uint32_t cna, uint8_t *bytes, unsigned size, bool strict, bool write)
{ assert(dev && address == 0x700000000000ULL && token == 97 && cna == 7 && strict);
  if (write) { writes++; assert(bytes[0] == 0x6b); }
  else { reads++; memset(bytes, 0x5a, size); }
  return io_error; }
static MemTxResult ubc_sim_dec_remote_write(void *dev, uint64_t address, uint32_t token,
    uint32_t cna, uint8_t *bytes, unsigned size)
{ (void)dev; (void)address; (void)token; (void)cna; (void)bytes; (void)size;
  assert(!"unexpected legacy retry"); return MEMTX_DECODE_ERROR; }
static uint64_t ldq_le_p(const uint8_t *bytes) { uint64_t v; memcpy(&v, bytes, 8); return v; }
static uint32_t ldl_le_p(const uint8_t *bytes) { uint32_t v; memcpy(&v, bytes, 4); return v; }
static uint16_t lduw_le_p(const uint8_t *bytes) { uint16_t v; memcpy(&v, bytes, 2); return v; }
static void stq_le_p(uint8_t *bytes, uint64_t value) { memcpy(bytes, &value, 8); }
static void stl_le_p(uint8_t *bytes, uint32_t value) { memcpy(bytes, &value, 4); }
static void stw_le_p(uint8_t *bytes, uint16_t value) { memcpy(bytes, &value, 2); }
""" + functions + r"""
int main(void) {
  GsvaRouteEntry route = {.key = {.size = 4096}, .token = {2, 3},
      .home_cna = 7, .backing_token_id = 97, .map_id = 1, .remote_uba = 0x700000000000ULL};
  uint64_t value = 0;
  typeof(*decoder.bcs) bcs = {.ubc_dev = &route}; decoder.bcs = &bcs;
  assert(sim_dec_gsva_cpu_window_read(&route, 0, &value, 8, 0) == MEMTX_OK);
  assert(value == 0x5a5a5a5a5a5a5a5aULL && reads == 1);
  assert(sim_dec_gsva_cpu_window_write(&route, 0, 0x6b, 8, 0) == MEMTX_OK && writes == 1);
  io_error = MEMTX_DECODE_ERROR;
  assert(sim_dec_gsva_cpu_window_read(&route, 0, &value, 8, 0) == io_error);
  assert(sim_dec_gsva_cpu_window_write(&route, 0, 0x6b, 8, 0) == io_error);
  token_error = 1;
  assert(sim_dec_gsva_cpu_window_read(&route, 0, &value, 8, 0) == MEMTX_ACCESS_ERROR);
  assert(sim_dec_gsva_cpu_window_write(&route, 0, 0x6b, 8, 0) == MEMTX_ACCESS_ERROR);
  assert(sim_dec_gsva_cpu_window_read(&route, UINT64_MAX, &value, 8, 0) == MEMTX_DECODE_ERROR);
  assert(sim_dec_gsva_cpu_window_write(&route, 4093, 0x6b, 8, 0) == MEMTX_DECODE_ERROR);
  assert(reads == 2 && writes == 2);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "cpu-window"
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-", "-o", str(binary)], input=program, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_strict_io_receiver_denies_failed_translation(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (QEMU / "ub_ubc.c").read_text()
        header = (ROOT / "vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h").read_text()
        declarations = []
        for name in ("UBCSimDecReadReqPld", "UBCGsvaIoReq", "UBCSimDecReadRespPldHdr"):
            declarations.append(re.search(r"typedef struct QEMU_PACKED " + name +
                                         r" \{.*?\n\} " + name + ";", header, re.S)[0])
        function = source.split("void ubc_handle_gsva_io(", 1)[1].split(
            "\nbool ubc_void_response_policy_enabled", 1)[0]
        program = r"""
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define QEMU_PACKED __attribute__((packed))
#define UBC_GSVA_IO_MAGIC 0x4753494fU
#define UBC_SIM_DEC_READ_STATUS_SUCCESS 0
#define UBC_SIM_DEC_READ_STATUS_ERROR 1
#define UBC_MSG_SUB_SIM_DEC_READ_RESP 3
typedef int MemTxResult;
#define MEMTX_OK 0
typedef struct { bool ummu; } BusControllerDev;
typedef struct { int unused; } UBLinkState;
static UBLinkState link;
static unsigned transfers, replies;
static int dma_error;
static uint8_t output[32];
static UBLinkState *ubc_find_active_link(BusControllerDev *dev, uint32_t *peer)
{ (void)dev; assert(*peer == 8); return &link; }
static int ubc_dma_read_local_data_tid_strict(BusControllerDev *dev, uint64_t address,
                                           void *bytes, uint32_t len, uint32_t token)
{ (void)dev; assert(address == 0x700000000000ULL && token == 97 && len == 8);
  transfers++; memset(bytes, 0x5a, len); return dma_error; }
static int ubc_dma_write_local_data_tid_strict(BusControllerDev *dev, uint64_t address,
                                            const void *bytes, uint32_t len, uint32_t token)
{ (void)dev; assert(address == 0x700000000000ULL && token == 97 && len == 8);
  assert(*(const uint8_t *)bytes == 0x6b); transfers++; return dma_error; }
static int ubc_send_msg_over_link(BusControllerDev *dev, UBLinkState *active,
                                 uint32_t peer, uint32_t sub, const void *bytes, uint32_t len)
{ (void)dev; assert(active == &link && peer == 8 && sub == 3 && len <= sizeof(output));
  replies++; memset(output, 0, sizeof(output)); memcpy(output, bytes, len); return 0; }
""" + "\n".join(declarations) + "\nvoid ubc_handle_gsva_io(" + function + r"""
int main(void) {
  BusControllerDev dev = {.ummu = true};
  uint8_t bytes[sizeof(UBCGsvaIoReq) + 8] = {0};
  UBCGsvaIoReq *io = (UBCGsvaIoReq *)bytes;
  UBCSimDecReadRespPldHdr *response = (UBCSimDecReadRespPldHdr *)output;
  *io = (UBCGsvaIoReq){.magic = UBC_GSVA_IO_MAGIC, .version = 1,
      .request = {.req_id = 7, .token_id = 97, .remote_uba = 0x700000000000ULL, .read_len = 8}};
  ubc_handle_gsva_io(&dev, bytes, sizeof(*io), 8);
  assert(transfers == 1 && replies == 1 && response->status == 0 && response->data_len == 8);
  assert(output[sizeof(*response)] == 0x5a);
  dma_error = 1;
  ubc_handle_gsva_io(&dev, bytes, sizeof(*io), 8);
  assert(transfers == 2 && replies == 2 && response->status == 1 && response->data_len == 0);
  io->write = 1; memset(bytes + sizeof(*io), 0x6b, 8);
  ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  assert(transfers == 3 && replies == 3 && response->status == 1);
  dma_error = 0;
  ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  assert(transfers == 4 && replies == 4 && response->status == 0 && response->data_len == 8);
  io->version = 2; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8); io->version = 1;
  io->reserved = 1; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8); io->reserved = 0;
  io->request.token_id = 0; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  io->request.token_id = UINT32_MAX; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  io->request.token_id = 97;
  io->request.read_len = 9; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  io->request.read_len = 8;
  ubc_handle_gsva_io(&dev, bytes, sizeof(bytes) - 1, 8);
  dev.ummu = false; ubc_handle_gsva_io(&dev, bytes, sizeof(bytes), 8);
  assert(transfers == 4 && replies == 4);
  return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "receiver"
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-", "-o", str(binary)], input=program, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_v2_layout_preserves_v1_and_separates_tokens(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        kernel = (KERNEL / "ub_sim_decoder.h").read_text()
        qemu = (QEMU / "ub_ubc.c").read_text()
        key = (QEMU / "gsva_key.h").read_text()
        declarations = []
        for name in ("sim_dec_gsva_key_v1", "sim_dec_gsva_map_req", "sim_dec_gsva_map_v2_req"):
            declarations.append(re.search(r"struct " + name + r" \{.*?\n\};", kernel, re.S)[0])
        for name, source in (("GsvaKeyV1", key), ("SimDecGsvaMapReq", qemu),
                             ("SimDecGsvaMapV2Req", qemu)):
            declarations.append(re.search(r"typedef struct QEMU_PACKED " + name +
                                         r" \{.*?\n\} " + name + ";", source, re.S)[0])
        program = """
#include <stdint.h>
#include <stddef.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#define QEMU_PACKED __attribute__((packed))
""" + "\n".join(declarations) + """
_Static_assert(sizeof(SimDecGsvaMapReq) == 140, "old QEMU payload");
_Static_assert(sizeof(struct sim_dec_gsva_map_req) == 144, "old kernel payload");
_Static_assert(sizeof(SimDecGsvaMapV2Req) == 152, "new payload");
_Static_assert(sizeof(struct sim_dec_gsva_map_v2_req) == 152, "matching payload");
_Static_assert(offsetof(SimDecGsvaMapV2Req, export_token_id) == 144, "export token");
_Static_assert(offsetof(struct sim_dec_gsva_map_v2_req, export_token_id) == 144,
               "kernel export token");
_Static_assert(offsetof(SimDecGsvaMapReq, token_id) ==
               offsetof(struct sim_dec_gsva_map_req, token_id), "lease token");
"""
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-c", "-", "-o", str(pathlib.Path(directory) / "layout.o")],
                                    input=program, text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
