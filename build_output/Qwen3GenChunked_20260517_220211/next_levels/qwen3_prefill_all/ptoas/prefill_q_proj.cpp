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

__global__ AICORE void prefill_q_proj(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ bfloat16_t* v3, int32_t v4, int32_t v5) {
  unsigned v6 = 0;
  const int32_t v7 = 40;
  const int32_t v8 = 128;
  const int32_t v9 = 80;
  const int32_t v10 = 4;
  const int32_t v11 = 0;
  const int32_t v12 = 1;
  const int32_t v13 = 5120;
  const int32_t v14 = 64;
  const int64_t v15 = 16384;
  const int64_t v16 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v17 = (size_t) v12;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  for (size_t v18 = (size_t) v11; v18 < ((size_t) v10); v18 += v17) {
    int32_t v19 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v4 * (uint32_t) v10) + (uint32_t) ((int32_t) v18));
    __gm__ float* v20;
    if (v19 < v9) {
      int32_t v21 = (int32_t) ((uint32_t) v19 * (uint32_t) v14);
      Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v22 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
      TASSIGN(v22, v16);
      Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
      __cbuf__ bfloat16_t* v24 = v22.data();
      uint64_t v25 = reinterpret_cast<uint64_t>(v24);
      TASSIGN(v23, v25);
      pto::Shape<1, 1, 1, 64, 128> v26 = pto::Shape<1, 1, 1, 64, 128>();
      pto::Stride<327680, 327680, 327680, 5120, 1> v27 = pto::Stride<327680, 327680, 327680, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v28 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v6 + v6 * (unsigned) v13 + v6 * (unsigned) v12), v26, v27);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v23, v28);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
      TASSIGN(v29, v15);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
      __cbuf__ bfloat16_t* v31 = v29.data();
      uint64_t v32 = reinterpret_cast<uint64_t>(v31);
      TASSIGN(v30, v32);
      pto::Shape<1, 1, 1, 128, 64> v33 = pto::Shape<1, 1, 1, 128, 64>();
      pto::Stride<655360, 655360, 655360, 5120, 1> v34 = pto::Stride<655360, 655360, 655360, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v35 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v3 + (v6 + (unsigned) v5 * (unsigned) v13 + (unsigned) v21 * (unsigned) v12), v33, v34);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      TLOAD(v30, v35);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
      TASSIGN(v36, v16);
      Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
      __ca__ bfloat16_t* v38 = v36.data();
      uint64_t v39 = reinterpret_cast<uint64_t>(v38);
      TASSIGN(v37, v39);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TMOV(v37, v23);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
      TASSIGN(v40, v16);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
      __cb__ bfloat16_t* v42 = v40.data();
      uint64_t v43 = reinterpret_cast<uint64_t>(v42);
      TASSIGN(v41, v43);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      TMOV(v41, v30);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v14);
      TASSIGN(v44, v16);
      Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v14);
      __cc__ float* v46 = v44.data();
      uint64_t v47 = reinterpret_cast<uint64_t>(v46);
      TASSIGN(v45, v47);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v45, v37, v41);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      for (size_t v48 = v17; v48 < ((size_t) v7); v48 += v17) {
        int32_t v49 = (int32_t) ((uint32_t) ((int32_t) v48) * (uint32_t) v8);
        Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
        TASSIGN(v50, v16);
        Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Mat, bfloat16_t, 64, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
        __cbuf__ bfloat16_t* v52 = v50.data();
        uint64_t v53 = reinterpret_cast<uint64_t>(v52);
        TASSIGN(v51, v53);
        pto::Shape<1, 1, 1, 64, 128> v54 = pto::Shape<1, 1, 1, 64, 128>();
        pto::Stride<327680, 327680, 327680, 5120, 1> v55 = pto::Stride<327680, 327680, 327680, 5120, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v56 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v6 + v6 * (unsigned) v13 + (unsigned) v49 * (unsigned) v12), v54, v55);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        TLOAD(v51, v56);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
        TASSIGN(v57, v15);
        Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
        __cbuf__ bfloat16_t* v59 = v57.data();
        uint64_t v60 = reinterpret_cast<uint64_t>(v59);
        TASSIGN(v58, v60);
        pto::Shape<1, 1, 1, 128, 64> v61 = pto::Shape<1, 1, 1, 128, 64>();
        pto::Stride<655360, 655360, 655360, 5120, 1> v62 = pto::Stride<655360, 655360, 655360, 5120, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND> v63 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<655360, 655360, 655360, 5120, 1>, pto::Layout::ND>(v3 + (v6 + (unsigned) ((int32_t) (uint32_t) v5 + (uint32_t) v49) * (unsigned) v13 + (unsigned) v21 * (unsigned) v12), v61, v62);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        TLOAD(v58, v63);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
        TASSIGN(v64, v16);
        Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Left, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v8);
        __ca__ bfloat16_t* v66 = v64.data();
        uint64_t v67 = reinterpret_cast<uint64_t>(v66);
        TASSIGN(v65, v67);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
        TMOV(v65, v51);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
        TASSIGN(v68, v16);
        Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v14);
        __cb__ bfloat16_t* v70 = v68.data();
        uint64_t v71 = reinterpret_cast<uint64_t>(v70);
        TASSIGN(v69, v71);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        TMOV(v69, v58);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v14);
        TASSIGN(v72, v16);
        Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Acc, float, 64, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v14);
        __cc__ float* v74 = v72.data();
        uint64_t v75 = reinterpret_cast<uint64_t>(v74);
        TASSIGN(v73, v75);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        TMATMUL_ACC(v73, v73, v65, v69);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
      };
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      pto::Shape<1, 1, 1, 64, 64> v76 = pto::Shape<1, 1, 1, 64, 64>();
      pto::Stride<327680, 327680, 327680, 5120, 1> v77 = pto::Stride<327680, 327680, 327680, 5120, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 64, 64>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v78 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 64>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v13 + (unsigned) v21 * (unsigned) v12), v76, v77);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v78, v45);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v20 = v1;
    } else {
      v20 = v1;
    };
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
