"""Execute managed view release using the production QEMU handler, no guest."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]


class GsvaViewReleaseTests(unittest.TestCase):
    def test_remote_retire_receipt_requires_quarantine_and_drain(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c").read_text()
        function = source.split("void gsva_coh_handle_rx_retire(", 1)[1].split(
            "\n}\n\nvoid gsva_coh_handle_rx_retire_ack", 1
        )[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#define GSVA_OK 0
#define GSVA_ERR_BAD_VERSION -1
#define GSVA_ERR_KEY_MISMATCH -2
#define GSVA_ERR_STALE_EPOCH -3
#define GSVA_ERR_ROUTE_MISSING -5
#define GSVA_ERR_COH_PENDING -6
#define GSVA_ERR_COH_TIMEOUT -7
#define GSVA_COH_RETIRED 4
#define GSVA_COH_TIMEOUT 5
#define GSVA_COH_MSG_RETIRE 9
#define UBC_MSG_SUB_GSVA_COH 1
#define qemu_log(...) ((void)0)
typedef struct { uint32_t version, flags; uint64_t segment_id, home_va, size,
    vmid, asid, pte_offset; uint32_t p_tag, cache_policy; uint64_t epoch; } GsvaKeyV1;
typedef struct { uint32_t version, op; uint64_t seq; uint32_t source_cna,
    target_cna; GsvaKeyV1 key; uint64_t access_va, access_len;
    uint32_t access_flags, error; } GsvaCohMsgV1;
typedef struct { GsvaKeyV1 key; int state; uint32_t owner_cna;
    bool pending; uint64_t pending_start_ms; unsigned sharers; } GsvaCohObject;
typedef struct { int unused; } GsvaCohTable;
typedef struct { struct { uint32_t cna; } parent; } BusControllerDev;
static GsvaCohTable table, *g_gsva_coh_default_table=&table;
static GsvaCohObject object, *lookup_result=&object;
static unsigned quarantines, drains, receipts, stage;
static int key_result, quarantine_result, drain_result;
static uint32_t last_error;
static int gsva_key_validate(const GsvaKeyV1 *key)
{ return key ? key_result : GSVA_ERR_BAD_VERSION; }
static GsvaCohObject *gsva_coh_lookup(GsvaCohTable *tbl, const GsvaKeyV1 *key)
{ assert(tbl==&table && key); return lookup_result; }
static int ubc_gsva_quarantine_local_holder(BusControllerDev *dev,
    const GsvaKeyV1 *key, uint32_t home)
{ assert(dev && key && home==7 && stage==0); quarantines++; stage=1;
  return quarantine_result; }
static int ubc_gsva_drain_local_holder(BusControllerDev *dev,
    const GsvaKeyV1 *key, uint32_t home)
{ assert(dev && key && home==7 && stage==1); drains++; stage=2;
  return drain_result; }
static void gsva_coh_sharers_clear(GsvaCohObject *obj)
{ assert(obj==&object); obj->sharers=0; }
static void gsva_coh_pending_clear(GsvaCohObject *obj)
{ assert(obj==&object); }
static void gsva_coh_send_ack(BusControllerDev *dev, const GsvaCohMsgV1 *msg,
    uint8_t subcode, uint32_t error)
{ assert(dev && msg && subcode==UBC_MSG_SUB_GSVA_COH);
  assert(stage==1 || stage==2 || !quarantines); receipts++; last_error=error; stage=3; }
void gsva_coh_handle_rx_retire(''' + function + r'''
}
static GsvaCohMsgV1 request(GsvaKeyV1 key)
{ return (GsvaCohMsgV1){.version=1,.op=GSVA_COH_MSG_RETIRE,.seq=4,
    .source_cna=7,.target_cna=8,.key=key,.access_va=key.home_va,
    .access_len=key.size}; }
static void reset(GsvaKeyV1 key)
{ memset(&object,0,sizeof(object)); object.key=key; object.state=1;
  object.owner_cna=8; object.sharers=1; lookup_result=&object;
  quarantines=drains=receipts=stage=0; key_result=quarantine_result=drain_result=0;
  last_error=0; }
int main(void) {
  GsvaKeyV1 key={.version=1,.segment_id=9,.home_va=4096,.size=4096,.epoch=3};
  GsvaCohMsgV1 msg=request(key); BusControllerDev dev={.parent={8}};
  reset(key); gsva_coh_handle_rx_retire(&dev,&msg);
  assert(stage==3 && quarantines==1 && drains==1 && receipts==1 && !last_error);
  assert(object.state==GSVA_COH_RETIRED && !object.owner_cna && !object.sharers);
  reset(key); object.pending=true; gsva_coh_handle_rx_retire(&dev,&msg);
  assert(quarantines==1 && !drains && (int32_t)last_error==GSVA_ERR_COH_PENDING);
  assert(object.state==1);
  reset(key); drain_result=GSVA_ERR_COH_TIMEOUT;
  gsva_coh_handle_rx_retire(&dev,&msg);
  assert(quarantines==1 && drains==1 && (int32_t)last_error==GSVA_ERR_COH_TIMEOUT);
  assert(object.state==1);
  reset(key); msg.key.epoch++; gsva_coh_handle_rx_retire(&dev,&msg);
  assert(!quarantines && !drains && (int32_t)last_error==GSVA_ERR_STALE_EPOCH);
  reset(key); msg=request(key); msg.access_len--;
  gsva_coh_handle_rx_retire(&dev,&msg);
  assert(!quarantines && !drains && (int32_t)last_error==GSVA_ERR_KEY_MISMATCH);
  reset(key); msg=request(key); msg.target_cna=9;
  gsva_coh_handle_rx_retire(&dev,&msg);
  assert(!receipts && !quarantines && !drains);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "retire-receipt"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                 "-o", str(binary)], input=program, text=True,
                capture_output=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_remote_retire_quarantines_drains_and_keeps_tombstone(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        block = source.split("static bool sim_dec_gsva_key_exact(", 1)[1].split(
            "\nint ubc_gsva_force_revoke_local_holder", 1
        )[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define GSVA_OK 0
#define GSVA_ERR_BAD_VERSION -1
#define GSVA_ERR_KEY_MISMATCH -2
#define GSVA_ERR_STALE_EPOCH -3
#define GSVA_ERR_ROUTE_MISSING -5
#define GSVA_ERR_COH_PENDING -6
#define GSVA_ERR_COH_TIMEOUT -7
#define GSVA_ERR_FEATURE_MISSING -12
#define GSVA_ROUTE_ACTIVE 1
#define GSVA_ROUTE_STALE 2
#define GSVA_ADDRESS_PROFILE_STRICT_GSVA 1
typedef struct { uint32_t version, flags; uint64_t segment_id, home_va, size,
    vmid, asid, pte_offset; uint32_t p_tag, cache_policy; uint64_t epoch; } GsvaKeyV1;
typedef struct { GsvaKeyV1 key; int state; uint32_t home_cna, address_profile,
    backing_token_id; uint64_t map_id; bool cpu_window_initialized,
    cpu_window_mapped; } GsvaRouteEntry;
typedef struct { int unused; } GsvaRouteTable;
typedef struct { struct { uint32_t cna; } parent; uint32_t gsva_strict_io_depth;
    bool gsva_unmap_in_progress; } BusControllerDev;
static GsvaRouteTable g_gsva_routes;
static int g_gsva_stats;
static GsvaRouteEntry *active, *tombstone;
static unsigned flushes, fences, invalidations, unmaps, stats_calls;
static int fence_result, key_result;
static bool kept_tombstone, stats_success;
static int gsva_key_validate(const GsvaKeyV1 *key)
{ return key ? key_result : GSVA_ERR_BAD_VERSION; }
static void gsva_tables_init(void) {}
static GsvaRouteEntry *gsva_route_lookup_base(GsvaRouteTable *table,
    const GsvaKeyV1 *key) { (void)table; (void)key; return active; }
static GsvaRouteEntry *gsva_route_lookup_tombstone(GsvaRouteTable *table,
    const GsvaKeyV1 *key) { (void)table; (void)key; return tombstone; }
static void gsva_tlb_stable_flush_key(const GsvaKeyV1 *key, const char *reason)
{ assert(key && reason); flushes++; }
static int obmm_coh_send_fence(BusControllerDev *dev, uint32_t home,
    uint64_t start, uint64_t size, uint32_t token)
{ assert(dev && dev->gsva_unmap_in_progress && home==7 && start==4096 &&
         size==4096 && token==97); fences++; return fence_result; }
static void obmm_coh_invalidate_local_range(BusControllerDev *dev,
    uint32_t home, uint64_t start, uint64_t size, uint32_t token)
{ assert(dev && home==7 && start==4096 && size==4096 && token==97);
  invalidations++; }
static int gsva_route_unmap(GsvaRouteTable *table, uint64_t map_id, bool keep)
{ (void)table; assert(active && map_id==42 && keep); unmaps++;
  kept_tombstone=keep; active->cpu_window_mapped=false; tombstone=active;
  active=0; return GSVA_OK; }
static void gsva_stats_unmap(int *stats, bool success)
{ assert(stats==&g_gsva_stats); stats_calls++; stats_success=success; }
static bool sim_dec_gsva_key_exact(''' + block + r'''
int main(void) {
  GsvaKeyV1 key={.version=1,.segment_id=9,.home_va=4096,.size=4096,.epoch=3};
  GsvaRouteEntry route={.key=key,.state=GSVA_ROUTE_ACTIVE,.home_cna=7,
      .address_profile=GSVA_ADDRESS_PROFILE_STRICT_GSVA,.backing_token_id=97,
      .map_id=42,.cpu_window_initialized=true,.cpu_window_mapped=true};
  BusControllerDev dev={.parent={8},.gsva_strict_io_depth=1};
  active=&route;
  assert(ubc_gsva_drain_local_holder(&dev,&key,7)==GSVA_ERR_COH_PENDING);
  assert(route.state==GSVA_ROUTE_STALE && flushes==1 && !fences && !unmaps);
  dev.gsva_strict_io_depth=0; fence_result=-1;
  assert(ubc_gsva_drain_local_holder(&dev,&key,7)==GSVA_ERR_COH_TIMEOUT);
  assert(fences==1 && !invalidations && !unmaps && !dev.gsva_unmap_in_progress);
  fence_result=0;
  assert(!ubc_gsva_drain_local_holder(&dev,&key,7));
  assert(fences==2 && invalidations==1 && unmaps==1 && kept_tombstone);
  assert(stats_calls==1 && stats_success && !dev.gsva_unmap_in_progress);
  assert(!ubc_gsva_drain_local_holder(&dev,&key,7));
  assert(fences==2 && unmaps==1);
  GsvaKeyV1 stale=key; stale.epoch++;
  assert(ubc_gsva_quarantine_local_holder(&dev,&stale,7)==GSVA_ERR_STALE_EPOCH);
  assert(ubc_gsva_quarantine_local_holder(&dev,&key,6)==GSVA_ERR_KEY_MISMATCH);
  tombstone=0; active=&route; route.cpu_window_mapped=true;
  route.backing_token_id=0; route.state=GSVA_ROUTE_ACTIVE;
  assert(ubc_gsva_quarantine_local_holder(&dev,&key,7)==GSVA_ERR_FEATURE_MISSING);
  assert(route.state==GSVA_ROUTE_STALE);
  route.backing_token_id=97; dev.gsva_unmap_in_progress=true;
  assert(ubc_gsva_drain_local_holder(&dev,&key,7)==GSVA_ERR_COH_PENDING);
  assert(ubc_gsva_quarantine_local_holder(0,&key,7)!=0);
  key_result=GSVA_ERR_KEY_MISMATCH; dev.gsva_unmap_in_progress=false;
  assert(ubc_gsva_quarantine_local_holder(&dev,&key,7)==GSVA_ERR_BAD_VERSION);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "remote-retire"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                 "-o", str(binary)], input=program, text=True,
                capture_output=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_local_revoke_requires_exact_identity_and_physical_drain(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires C compiler")
        source = (ROOT / "vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c").read_text()
        kernel_uapi = (
            ROOT / "guest-linux/kernel_ub/include/uapi/ub/gsva.h"
        ).read_text()
        self.assertIn("#define OBMM_GSVA_EVENT_LOCAL_REVOKE\t8", kernel_uapi)
        event_handler = source.split("case SIM_DEC_OP_GSVA_EVENT_V1:", 1)[1].split(
            "\n    default:", 1
        )[0]
        self.assertIn("case 8: /* LocalRevoke */", event_handler)
        self.assertIn("ubc_gsva_force_revoke_local_holder(", event_handler)
        function = source.split("int ubc_gsva_force_revoke_local_holder(", 1)[1].split(
            "\n}\n\n/*\n * ubc_handle_sim_dec_message", 1
        )[0]
        program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#define GSVA_OK 0
#define GSVA_ERR_BAD_VERSION -1
#define GSVA_ERR_KEY_MISMATCH -2
#define GSVA_ERR_STALE_EPOCH -3
#define GSVA_ERR_TOKEN_DENIED -4
#define GSVA_ERR_ROUTE_MISSING -5
#define GSVA_ERR_COH_PENDING -6
#define GSVA_ERR_COH_TIMEOUT -7
#define GSVA_ERR_FEATURE_MISSING -12
#define GSVA_ROUTE_ACTIVE 1
#define GSVA_ROUTE_STALE 2
#define GSVA_ROUTE_RETIRED 3
#define GSVA_COH_RETIRED 4
#define GSVA_COH_TIMEOUT 5
#define qemu_log(...) ((void)0)
typedef struct { uint32_t version, flags; uint64_t segment_id, home_va, size,
    vmid, asid, pte_offset; uint32_t p_tag, cache_policy; uint64_t epoch; } GsvaKeyV1;
typedef struct { uint32_t token_id, token_value; } Token;
typedef struct { GsvaKeyV1 key; int state; uint32_t home_cna; Token token;
    bool cpu_window_mapped; } GsvaRouteEntry;
typedef struct { GsvaKeyV1 key; int state; uint32_t owner_cna;
    uint64_t sharer_bitmap; uint32_t sharer_count, sharer_cnas[4];
    bool pending; uint64_t pending_seq; uint32_t pending_op, pending_target;
    uint64_t pending_ack_bitmap; uint32_t pending_ack_count, pending_ack_cnas[4];
    uint64_t pending_start_ms; } GsvaCohObject;
typedef struct { int unused; } GsvaRouteTable;
typedef struct { int unused; } GsvaCohTable;
typedef struct { struct { uint32_t cna; } parent; } BusControllerDev;
static GsvaRouteTable g_gsva_routes;
static GsvaCohTable g_gsva_coh;
static GsvaRouteEntry route, tombstone, *active_route, *retired_route;
static GsvaCohObject object, *coh_object;
static unsigned quarantines, drains;
static int key_result, quarantine_result, drain_result;
static int gsva_key_validate(const GsvaKeyV1 *key)
{ return key ? key_result : GSVA_ERR_BAD_VERSION; }
static bool sim_dec_gsva_key_exact(const GsvaKeyV1 *a, const GsvaKeyV1 *b)
{ return a && b && !memcmp(a, b, sizeof(*a)); }
static void gsva_tables_init(void) {}
static GsvaRouteEntry *gsva_route_lookup_base(GsvaRouteTable *table,
    const GsvaKeyV1 *key) { assert(table == &g_gsva_routes && key); return active_route; }
static GsvaRouteEntry *gsva_route_lookup_tombstone(GsvaRouteTable *table,
    const GsvaKeyV1 *key) { assert(table == &g_gsva_routes && key); return retired_route; }
static GsvaCohObject *gsva_coh_lookup(GsvaCohTable *table, const GsvaKeyV1 *key)
{ assert(table == &g_gsva_coh && key); return coh_object; }
static int ubc_gsva_quarantine_local_holder(BusControllerDev *dev,
    const GsvaKeyV1 *key, uint32_t home)
{ assert(dev && key && home == 7); quarantines++; route.state = GSVA_ROUTE_STALE;
  return quarantine_result; }
static int ubc_gsva_drain_local_holder(BusControllerDev *dev,
    const GsvaKeyV1 *key, uint32_t home)
{ assert(dev && key && home == 7); drains++; return drain_result; }
int ubc_gsva_force_revoke_local_holder(''' + function + r'''
}
static void reset(GsvaKeyV1 key)
{ memset(&route, 0, sizeof(route)); route.key = key; route.state = GSVA_ROUTE_ACTIVE;
  route.home_cna = 7; route.token.token_id = 11; route.token.token_value = 53;
  route.cpu_window_mapped = true; memset(&object, 0, sizeof(object)); object.key = key;
  object.state = 1; object.owner_cna = 8; object.sharer_count = 2;
  object.sharer_bitmap = 3; object.pending_seq = 9; object.pending_op = 2;
  object.pending_target = 8; object.pending_ack_count = 1;
  object.pending_ack_bitmap = 1; object.pending_start_ms = 4;
  active_route = &route; retired_route = 0; coh_object = &object;
  quarantines = drains = 0; key_result = quarantine_result = drain_result = 0; }
int main(void) {
  GsvaKeyV1 key = {.version=1,.segment_id=9,.home_va=4096,.size=4096,.epoch=3};
  BusControllerDev dev = {.parent={8}};
  reset(key);
  assert(!ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53));
  assert(quarantines==1 && drains==1 && object.state==GSVA_COH_RETIRED);
  assert(!object.owner_cna && !object.sharer_count && !object.sharer_bitmap);
  assert(!object.pending && !object.pending_seq && !object.pending_ack_count);
  reset(key); assert(ubc_gsva_force_revoke_local_holder(&dev,&key,6,11,53)==GSVA_ERR_BAD_VERSION);
  assert(!quarantines && !drains);
  reset(key); assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,54)==GSVA_ERR_TOKEN_DENIED);
  assert(!quarantines && !drains);
  reset(key); object.pending=true;
  assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53)==GSVA_ERR_COH_PENDING);
  assert(quarantines==1 && !drains && object.state==1);
  reset(key); object.state=GSVA_COH_TIMEOUT;
  assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53)==GSVA_ERR_COH_TIMEOUT);
  assert(quarantines==1 && !drains);
  reset(key); drain_result=GSVA_ERR_COH_TIMEOUT;
  assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53)==GSVA_ERR_COH_TIMEOUT);
  assert(quarantines==1 && drains==1 && object.state==1);
  reset(key); active_route=0; tombstone=route; tombstone.state=GSVA_ROUTE_RETIRED;
  tombstone.cpu_window_mapped=false; retired_route=&tombstone;
  assert(!ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53));
  assert(!quarantines && !drains);
  reset(key); coh_object=0;
  assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53)==GSVA_ERR_ROUTE_MISSING);
  assert(!quarantines && !drains);
  reset(key); key.epoch++;
  assert(ubc_gsva_force_revoke_local_holder(&dev,&key,8,11,53)==GSVA_ERR_STALE_EPOCH);
  assert(!quarantines && !drains);
  return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "local-revoke"
            result = subprocess.run(
                [compiler, "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
                 "-o", str(binary)], input=program, text=True,
                capture_output=True, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(
                [str(binary)], capture_output=True, text=True, timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stderr)

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
typedef struct { int unused; } GsvaHomeIdentity;
typedef struct { uint32_t gsva_strict_io_depth; bool gsva_unmap_in_progress; } BusControllerDev;
static unsigned calls;
static bool nested;
static int result;
static MemTxResult ubc_sim_dec_remote_io(BusControllerDev *, uint64_t, uint32_t,
    uint32_t, uint8_t *, uint32_t, bool, bool, const GsvaHomeIdentity *);
static MemTxResult ubc_sim_dec_remote_io_body(BusControllerDev *dev, uint64_t addr,
    uint32_t token, uint32_t cna, uint8_t *data, uint32_t len, bool strict, bool write,
    const GsvaHomeIdentity *identity)
{
    calls++;
    assert(addr==4096 && token==97 && cna==7 && data && len==8);
    assert(dev->gsva_strict_io_depth==(strict ? (nested ? 2u : 1u) : 0u));
    /* Model receipt arrival and an RX callback before the outer call returns.
     * There is no per-response pending flag left at this point. */
    if (strict && !nested) {
        nested=true;
        assert(ubc_sim_dec_remote_io(dev,addr,token,cna,data,len,true,write,identity)==result);
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
        assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,i & 2,0)==result);
        assert(!dev.gsva_strict_io_depth);
    }
    assert(calls==8);
    dev.gsva_unmap_in_progress=true;
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,false,0)==MEMTX_ACCESS_ERROR);
    assert(!dev.gsva_strict_io_depth && calls==8);
    dev.gsva_unmap_in_progress=false; dev.gsva_strict_io_depth=UINT32_MAX;
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,true,false,0)==MEMTX_ACCESS_ERROR);
    assert(dev.gsva_strict_io_depth==UINT32_MAX && calls==8);
    dev.gsva_strict_io_depth=0;
    assert(ubc_sim_dec_remote_io(0,4096,97,7,data,8,true,false,0)==MEMTX_ACCESS_ERROR);
    assert(calls==8);
    assert(ubc_sim_dec_remote_io(&dev,4096,97,7,data,8,false,false,0)==result);
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
