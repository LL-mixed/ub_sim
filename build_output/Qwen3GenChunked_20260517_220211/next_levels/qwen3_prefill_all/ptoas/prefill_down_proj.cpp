#include "pto/pto-inst.hpp"
using namespace pto;

enum class PTOAutoSyncTailMode : int {
  kBarrierAll = 0,
  kSetWaitMte3ToSEvent0 = 1,
};

static AICORE inline void ptoas_auto_sync_tail(
    PTOAutoSyncTailMode mode = PTOAutoSyncTailMode::kBarrierAll) {
  switch (mode) {
  case PTOAutoSyncTailMode::kSetWaitMte3ToSEvent0:
    set_flag(PIPE_MTE3, PIPE_S, EVENT_ID0);
    wait_flag(PIPE_MTE3, PIPE_S, EVENT_ID0);
    break;
  case PTOAutoSyncTailMode::kBarrierAll:
  default:
    pipe_barrier(PIPE_ALL);
    break;
  }
}

__global__ AICORE void prefill_down_proj(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4, int32_t v5) {
  unsigned v6 = 0;
  const int32_t v7 = 136;
  const int32_t v8 = 128;
  const int32_t v9 = 5120;
  const int32_t v10 = 1;
  const int32_t v11 = 17408;
  const int32_t v12 = 64;
  const int64_t v13 = 16384;
  const int64_t v14 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v15 = (size_t) v10;
  Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
  TASSIGN(v16, v14);
  Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v17 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
  __cbuf__ bfloat16_t* v18 = v16.data();
  uint64_t v19 = reinterpret_cast<uint64_t>(v18);
  TASSIGN(v17, v19);
  pto::Shape<1, 1, 1, 64, 128> v20 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<1114112, 1114112, 1114112, 17408, 1> v21 = pto::Stride<1114112, 1114112, 1114112, 17408, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<1114112, 1114112, 1114112, 17408, 1>, pto::Layout::ND> v22 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<1114112, 1114112, 1114112, 17408, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v11 + v6 * (unsigned) v10), v20, v21);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  TLOAD(v17, v22);
  set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
  Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
  TASSIGN(v23, v13);
  Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
  __cbuf__ bfloat16_t* v25 = v23.data();
  uint64_t v26 = reinterpret_cast<uint64_t>(v25);
  TASSIGN(v24, v26);
  pto::Shape<1, 1, 1, 128, 128> v27 = pto::Shape<1, 1, 1, 128, 128>();
  pto::Stride<655360, 655360, 655360, 5120, 1> v28 = pto::Stride<655360, 655360, 655360, 5120, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 128>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v29 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 128>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v2 + (v6 + (unsigned) v4 * (unsigned) v9 + (unsigned) v5 * (unsigned) v10), v27, v28);
  TLOAD(v24, v29);
  set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
  Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
  TASSIGN(v30, v14);
  Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
  __ca__ bfloat16_t* v32 = v30.data();
  uint64_t v33 = reinterpret_cast<uint64_t>(v32);
  TASSIGN(v31, v33);
  wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
  TMOV(v31, v17);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v34 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
  TASSIGN(v34, v14);
  Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
  __cb__ bfloat16_t* v36 = v34.data();
  uint64_t v37 = reinterpret_cast<uint64_t>(v36);
  TASSIGN(v35, v37);
  wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
  TMOV(v35, v24);
  set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v12, v8);
  TASSIGN(v38, v14);
  Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v12, v8);
  __cc__ float* v40 = v38.data();
  uint64_t v41 = reinterpret_cast<uint64_t>(v40);
  TASSIGN(v39, v41);
  wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
  TMATMUL(v39, v31, v35);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  for (size_t v42 = v15; v42 < ((size_t) v7); v42 += v15) {
    int32_t v43 = (int32_t) ((uint32_t) ((int32_t) v42) * (uint32_t) v8);
    Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    TASSIGN(v44, v14);
    Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    __cbuf__ bfloat16_t* v46 = v44.data();
    uint64_t v47 = reinterpret_cast<uint64_t>(v46);
    TASSIGN(v45, v47);
    pto::Shape<1, 1, 1, 64, 128> v48 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<1114112, 1114112, 1114112, 17408, 1> v49 = pto::Stride<1114112, 1114112, 1114112, 17408, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<1114112, 1114112, 1114112, 17408, 1>, pto::Layout::ND> v50 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<1114112, 1114112, 1114112, 17408, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v11 + (unsigned) v43 * (unsigned) v10), v48, v49);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    TLOAD(v45, v50);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
    TASSIGN(v51, v13);
    Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Mat, bfloat16_t, 128, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
    __cbuf__ bfloat16_t* v53 = v51.data();
    uint64_t v54 = reinterpret_cast<uint64_t>(v53);
    TASSIGN(v52, v54);
    pto::Shape<1, 1, 1, 128, 128> v55 = pto::Shape<1, 1, 1, 128, 128>();
    pto::Stride<655360, 655360, 655360, 5120, 1> v56 = pto::Stride<655360, 655360, 655360, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 128>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v57 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 128>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v2 + (v6 + (unsigned) ((int32_t) (uint32_t) v4 + (uint32_t) v43) * (unsigned) v9 + (unsigned) v5 * (unsigned) v10), v55, v56);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    TLOAD(v52, v57);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    TASSIGN(v58, v14);
    Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    __ca__ bfloat16_t* v60 = v58.data();
    uint64_t v61 = reinterpret_cast<uint64_t>(v60);
    TASSIGN(v59, v61);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    TMOV(v59, v45);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
    TASSIGN(v62, v14);
    Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v8);
    __cb__ bfloat16_t* v64 = v62.data();
    uint64_t v65 = reinterpret_cast<uint64_t>(v64);
    TASSIGN(v63, v65);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    TMOV(v63, v52);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v66 = Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v12, v8);
    TASSIGN(v66, v14);
    Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Acc, float, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v12, v8);
    __cc__ float* v68 = v66.data();
    uint64_t v69 = reinterpret_cast<uint64_t>(v68);
    TASSIGN(v67, v69);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    TMATMUL_ACC(v67, v67, v59, v63);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  }
  set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
  pto::Shape<1, 1, 1, 64, 128> v70 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<8192, 8192, 8192, 128, 1> v71 = pto::Stride<8192, 8192, 8192, 128, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<8192, 8192, 8192, 128, 1>, pto::Layout::ND> v72 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<8192, 8192, 8192, 128, 1>, pto::Layout::ND>(v3 + (v6 + v6 * (unsigned) v8 + v6 * (unsigned) v10), v70, v71);
  wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
  TSTORE(v72, v39);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
