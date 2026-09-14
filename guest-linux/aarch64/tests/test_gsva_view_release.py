"""Execute managed view release using the production QEMU handler, no guest."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class GsvaViewReleaseTests(unittest.TestCase):
    def test_strict_io_guard_covers_complete_call_and_errors(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        function = source.rsplit("static MemTxResult ubc_sim_dec_remote_io(", 1)[1].split("\n}\n", 1)[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define MEMTX_ACCESS_ERROR 9
typedef int MemTxResult;
typedef struct { uint32_t gsva_strict_io_depth; bool gsva_unmap_in_progress; } BusControllerDev;
static unsigned calls;
static bool nested;
static int result;
static MemTxResult ubc_sim_dec_remote_io(BusControllerDev *, uint64_t, uint32_t,
    uint32_t, uint8_t *, uint32_t, bool, bool);
static MemTxResult ubc_sim_dec_remote_io_body(BusControllerDev *dev, uint64_t addr,
    uint32_t token, uint32_t cna, uint8_t *data, uint32_t len, bool strict, bool write)
{
    calls++;
    assert(addr==4096 && token==97 && cna==7 && data && len==8);
    assert(dev->gsva_strict_io_depth==(strict ? (nested ? 2u : 1u) : 0u));
    /* Model receipt arrival and an RX callback before the outer call returns.
     * There is no per-response pending flag left at this point. */
    if (strict && !nested) {
        nested=true;
        assert(ubc_sim_dec_remote_io(dev,addr,token,cna,data,len,true,write)==result);
        nested=false;
        assert(dev->gsva_strict_io_depth==1);
    }
    return result;
}
static MemTxResult ubc_sim_dec_remote_io(''' + function + r'''
}
int main(void) {
    BusControllerDev dev={0}; uint8_t data[8]={0};
    for (unsigned i=0;i<4;i++) {
        result=(i & 1) ? 17 : 0;
        assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,i & 2)==result);
        assert(!dev.gsva_strict_io_depth);
    }
    assert(calls==8);
    dev.gsva_unmap_in_progress=true;
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,false)==MEMTX_ACCESS_ERROR);
    assert(!dev.gsva_strict_io_depth && calls==8);
    dev.gsva_unmap_in_progress=false; dev.gsva_strict_io_depth=UINT32_MAX;
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,false)==MEMTX_ACCESS_ERROR);
    assert(dev.gsva_strict_io_depth==UINT32_MAX && calls==8);
    dev.gsva_strict_io_depth=0;
    assert(ubc_sim_dec_remote_io(0,4096,97,7,data,8,true,false)==MEMTX_ACCESS_ERROR);
    assert(calls==8);
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,false,false)==result);
    assert(calls==9 && !dev.gsva_strict_io_depth);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "io-guard"
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-", "-o", str(binary)], input=program, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_kernel_release_never_falls_back_after_gsva_failure(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "guest-linux/kernel_ub/drivers/ub/ubus/sim/ub_sim_decoder_main.c").read_text()
        function = source.split("static int ub_sim_decoder_obmm_unimport(", 1)[1].split("\n}\n", 1)[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define GSVA_OK 0
#define GSVA_ERR_ROUTE_MISSING 2
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
struct obmm_sim_dec_unimport_info { uint64_t map_id; uint32_t scna; bool is_gsva, managed_view; };
struct sim_dec_gsva_unmap_req { uint32_t version; uint64_t map_id; };
struct sim_dec_gsva_unmap_resp { int error; };
static struct { bool enabled; int service; } decoder = {.enabled=true}, *g_decoder=&decoder;
static int transport, response, legacy_calls;
static uint32_t version;
static int ub_sim_dec_backend_gsva_unmap_v1(void *dec, uint32_t cna,
    struct sim_dec_gsva_unmap_req *req, struct sim_dec_gsva_unmap_resp *resp)
{ (void)dec; assert(cna==7 && req->map_id==42); version=req->version;
  resp->error=response; return transport; }
static int ub_sim_decoder_unmap(int *service, uint64_t id)
{ (void)service; assert(id==42); legacy_calls++; return 0; }
static int ub_sim_decoder_obmm_unimport(''' + function + r'''
}
int main(void) {
  struct obmm_sim_dec_unimport_info info={42,7,true,true};
  assert(!ub_sim_decoder_obmm_unimport(&info) && version==2);
  response=9;
  assert(ub_sim_decoder_obmm_unimport(&info)==-EIO);
  response=0; transport=-ETIMEDOUT;
  assert(ub_sim_decoder_obmm_unimport(&info)==-ETIMEDOUT);
  response=GSVA_ERR_ROUTE_MISSING;
  assert(ub_sim_decoder_obmm_unimport(&info)==-ETIMEDOUT);
  transport=0;
  assert(!ub_sim_decoder_obmm_unimport(&info));
  info.managed_view=false; response=0;
  assert(!ub_sim_decoder_obmm_unimport(&info) && version==1 && !legacy_calls);
  info.is_gsva=false;
  assert(!ub_sim_decoder_obmm_unimport(&info) && legacy_calls==1);
  assert(ub_sim_decoder_obmm_unimport(0)==-EINVAL);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "kernel-release"
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-", "-o", str(binary)], input=program, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_release_preserves_object_and_failure_preserves_mapping(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        function = source.split("static int sim_dec_handle_gsva_unmap(", 1)[1].split("\n}\n", 1)[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define GSVA_OK 0
#define GSVA_ERR_BAD_VERSION 1
#define GSVA_ERR_ROUTE_MISSING 2
#define GSVA_ERR_FEATURE_MISSING 3
#define GSVA_ERR_COH_TIMEOUT 4
#define GSVA_ERR_COH_PENDING 5
#define GSVA_ROUTE_ACTIVE 1
#define GSVA_ROUTE_STALE 2
#define qemu_log(...) ((void)0)
#define OBJECT(p) (p)
typedef struct { uint64_t home_va, size; } Key;
typedef struct Entry { uint64_t map_id, local_pa; uint32_t backing_token_id, home_cna;
    struct { uint32_t token_id; } token; Key key; int cpu_window;
    bool cpu_window_mapped; int state; struct Entry *next; } GsvaRouteEntry;
typedef struct { uint32_t version; uint64_t map_id; } SimDecGsvaUnmapReq;
typedef struct { int error; } SimDecGsvaUnmapResp;
static struct { GsvaRouteEntry *routes; } g_gsva_routes;
static int g_gsva_coh, g_gsva_stats;
typedef struct { uint32_t gsva_strict_io_depth; bool gsva_unmap_in_progress; } BusControllerDev;
static struct { struct { BusControllerDev *ubc_dev; } *bcs; } decoder, *g_sim_decoder = &decoder;
#define QTAILQ_FOREACH(p, h, field) for ((p) = *(h); (p); (p) = (p)->field)
static unsigned retired, removed, fenced, invalidated, detached, unmapped, flushed;
static int fence_error, remove_error;
static bool tombstone;
static int sim_dec_handle_gsva_unmap(const SimDecGsvaUnmapReq *, SimDecGsvaUnmapResp *);
static void gsva_tables_init(void) {}
static int obmm_coh_send_fence(void *dev, uint32_t cna, uint64_t va, uint64_t size, uint32_t token)
{ assert(dev && cna == 7 && va == 4096 && size == 4096 && (token == 97 || token == 2));
  if (token == 97) {
    SimDecGsvaUnmapReq nested = {.version=2, .map_id=42};
    SimDecGsvaUnmapResp resp;
    assert(((BusControllerDev *)dev)->gsva_unmap_in_progress);
    assert(sim_dec_handle_gsva_unmap(&nested, &resp) && resp.error==GSVA_ERR_COH_PENDING);
    nested.version=1;
    assert(sim_dec_handle_gsva_unmap(&nested, &resp) && resp.error==GSVA_ERR_COH_PENDING);
  }
  fenced++; return fence_error; }
static void obmm_coh_invalidate_local_range(void *dev, uint32_t cna, uint64_t va, uint64_t size, uint32_t token)
{ (void)dev; (void)cna; (void)va; (void)size; (void)token; invalidated++; }
static int gsva_coh_object_remove(int *table, Key *key)
{ (void)table; (void)key; removed++; return remove_error; }
static int gsva_coh_retire(int *table, Key *key, int cna)
{ (void)table; (void)key; (void)cna; retired++; return 0; }
static void ubc_cpu_window_detach(int *window) { (void)window; detached++; }
static int gsva_route_unmap(void *table, uint64_t id, bool keep)
{ (void)table; assert(id == 42); unmapped++; tombstone = keep; return 0; }
static void gsva_stats_unmap(int *stats, bool ok) { (void)stats; (void)ok; }
static void sim_dec_flush_gva_tlbs(const char *reason) { (void)reason; flushed++; }
static int sim_dec_handle_gsva_unmap(''' + function + r'''
}
int main(void) {
  GsvaRouteEntry route = {.map_id=42, .backing_token_id=97, .home_cna=7,
      .token={2}, .key={4096,4096}, .cpu_window_mapped=true, .state=GSVA_ROUTE_ACTIVE};
  BusControllerDev dev = {.gsva_strict_io_depth=1};
  typeof(*decoder.bcs) bcs = {.ubc_dev=&dev}; decoder.bcs=&bcs;
  g_gsva_routes.routes=&route;
  SimDecGsvaUnmapReq req = {.version=2, .map_id=42}; SimDecGsvaUnmapResp resp;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==GSVA_ERR_COH_PENDING);
  assert(route.state==GSVA_ROUTE_STALE && route.cpu_window_mapped);
  assert(!fenced && !removed && !unmapped && !dev.gsva_unmap_in_progress);
  dev.gsva_strict_io_depth=0;
  fence_error=1;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==GSVA_ERR_COH_TIMEOUT);
  assert(!removed && !retired && !detached && !unmapped && route.cpu_window_mapped);
  assert(route.state==GSVA_ROUTE_STALE && !dev.gsva_unmap_in_progress);
  fence_error=0; remove_error=5;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==5);
  assert(!retired && !detached && !unmapped && route.cpu_window_mapped);
  assert(route.state==GSVA_ROUTE_STALE && !dev.gsva_unmap_in_progress);
  remove_error=0;
  assert(!sim_dec_handle_gsva_unmap(&req,&resp));
  assert(!retired && detached==1 && unmapped==1 && flushed==1 && !tombstone);
  assert(!dev.gsva_unmap_in_progress);
  route.cpu_window_mapped=true; req.version=1;
  assert(!sim_dec_handle_gsva_unmap(&req,&resp));
  assert(retired==1 && tombstone && detached==2 && unmapped==2);
  req.version=2; route.backing_token_id=0;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==GSVA_ERR_BAD_VERSION);
  g_gsva_routes.routes=0;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==GSVA_ERR_ROUTE_MISSING);
  req.version=3;
  assert(sim_dec_handle_gsva_unmap(&req,&resp) && resp.error==GSVA_ERR_BAD_VERSION);
  assert(unmapped==2 && flushed==2);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "release"
            result = subprocess.run([compiler, "-Wall", "-Wextra", "-Werror", "-x", "c",
                                     "-", "-o", str(binary)], input=program, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
