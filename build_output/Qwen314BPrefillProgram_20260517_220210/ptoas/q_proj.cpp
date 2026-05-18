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

__global__ AICORE void q_proj(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ bfloat16_t* v3, int32_t v4) {
  unsigned v5 = 0;
  const int32_t v6 = 40;
  const int32_t v7 = 128;
  const int32_t v8 = 4;
  const int32_t v9 = 1;
  const int32_t v10 = 5120;
  const int32_t v11 = 64;
  const int64_t v12 = 16384;
  const int64_t v13 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v14 = (size_t) v9;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  for (size_t v15 = (size_t) v4; v15 < ((size_t) ((int32_t) (uint32_t) v4 + (uint32_t) v8)); v15 += v14) {
    int32_t v16 = (int32_t) ((uint32_t) ((int32_t) v15) * (uint32_t) v11);
    Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v17 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
    TASSIGN(v17, v13);
    Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v18 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
    __cbuf__ bfloat16_t* v19 = v17.data();
    uint64_t v20 = reinterpret_cast<uint64_t>(v19);
    TASSIGN(v18, v20);
    pto::Shape<1, 1, 1, 64, 128> v21 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v22 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v23 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v10 + v5 * (unsigned) v9), v21, v22);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    TLOAD(v18, v23);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
    TASSIGN(v24, v12);
    Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
    __cbuf__ bfloat16_t* v26 = v24.data();
    uint64_t v27 = reinterpret_cast<uint64_t>(v26);
    TASSIGN(v25, v27);
    pto::Shape<1, 1, 1, 128, 64> v28 = pto::Shape<1, 1, 1, 128, 64>();
    pto::Stride<655360, 655360, 655360, 5120, 1> v29 = pto::Stride<655360, 655360, 655360, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v30 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v3 + (v5 + v5 * (unsigned) v10 + (unsigned) v16 * (unsigned) v9), v28, v29);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    TLOAD(v25, v30);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
    TASSIGN(v31, v13);
    Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
    __ca__ bfloat16_t* v33 = v31.data();
    uint64_t v34 = reinterpret_cast<uint64_t>(v33);
    TASSIGN(v32, v34);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    TMOV(v32, v18);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
    TASSIGN(v35, v13);
    Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
    __cb__ bfloat16_t* v37 = v35.data();
    uint64_t v38 = reinterpret_cast<uint64_t>(v37);
    TASSIGN(v36, v38);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    TMOV(v36, v25);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v11, v11);
    TASSIGN(v39, v13);
    Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v11, v11);
    __cc__ float* v41 = v39.data();
    uint64_t v42 = reinterpret_cast<uint64_t>(v41);
    TASSIGN(v40, v42);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
    TMATMUL(v40, v32, v36);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    for (size_t v43 = v14; v43 < ((size_t) v6); v43 += v14) {
      int32_t v44 = (int32_t) ((uint32_t) ((int32_t) v43) * (uint32_t) v7);
      Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
      TASSIGN(v45, v13);
      Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
      __cbuf__ bfloat16_t* v47 = v45.data();
      uint64_t v48 = reinterpret_cast<uint64_t>(v47);
      TASSIGN(v46, v48);
      pto::Shape<1, 1, 1, 64, 128> v49 = pto::Shape<1, 1, 1, 64, 128>();
      pto::Stride<327680, 327680, 327680, 5120, 1> v50 = pto::Stride<327680, 327680, 327680, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v51 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v10 + (unsigned) v44 * (unsigned) v9), v49, v50);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
      TLOAD(v46, v51);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
      TASSIGN(v52, v12);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
      __cbuf__ bfloat16_t* v54 = v52.data();
      uint64_t v55 = reinterpret_cast<uint64_t>(v54);
      TASSIGN(v53, v55);
      pto::Shape<1, 1, 1, 128, 64> v56 = pto::Shape<1, 1, 1, 128, 64>();
      pto::Stride<655360, 655360, 655360, 5120, 1> v57 = pto::Stride<655360, 655360, 655360, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v58 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v3 + (v5 + (unsigned) v44 * (unsigned) v10 + (unsigned) v16 * (unsigned) v9), v56, v57);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
      TLOAD(v53, v58);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
      Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
      TASSIGN(v59, v13);
      Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v7);
      __ca__ bfloat16_t* v61 = v59.data();
      uint64_t v62 = reinterpret_cast<uint64_t>(v61);
      TASSIGN(v60, v62);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
      TMOV(v60, v46);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
      TASSIGN(v63, v13);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v7, v11);
      __cb__ bfloat16_t* v65 = v63.data();
      uint64_t v66 = reinterpret_cast<uint64_t>(v65);
      TASSIGN(v64, v66);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
      TMOV(v64, v53);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
      Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v11, v11);
      TASSIGN(v67, v13);
      Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v11, v11);
      __cc__ float* v69 = v67.data();
      uint64_t v70 = reinterpret_cast<uint64_t>(v69);
      TASSIGN(v68, v70);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      TMATMUL_ACC(v68, v68, v60, v64);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    };
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    pto::Shape<1, 1, 1, 64, 64> v71 = pto::Shape<1, 1, 1, 64, 64>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v72 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 64, 64>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v73 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 64>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v10 + (unsigned) v16 * (unsigned) v9), v71, v72);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(v73, v40);
    set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  }
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
