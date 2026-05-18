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

__global__ AICORE void rms_lmhead_lm_head(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ bfloat16_t* v3, int32_t v4) {
  unsigned v5 = 0;
  const int32_t v6 = 128;
  const int32_t v7 = 64;
  const int32_t v8 = 2376;
  const int32_t v9 = 8;
  const int32_t v10 = 0;
  const int32_t v11 = 1024;
  const int32_t v12 = 1;
  const int32_t v13 = 152064;
  const int32_t v14 = 16;
  const int64_t v15 = 4096;
  const int64_t v16 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v17 = (size_t) v12;
  size_t v18 = (size_t) v9;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  for (size_t v19 = (size_t) v10; v19 < v18; v19 += v17) {
    int32_t v20 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v4 * (uint32_t) v9) + (uint32_t) ((int32_t) v19));
    __gm__ float* v21;
    if (v20 < v8) {
      int32_t v22 = (int32_t) ((uint32_t) v20 * (uint32_t) v7);
      Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
      TASSIGN(v23, v16);
      Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
      __cbuf__ bfloat16_t* v25 = v23.data();
      uint64_t v26 = reinterpret_cast<uint64_t>(v25);
      TASSIGN(v24, v26);
      pto::Shape<1, 1, 1, 16, 128> v27 = pto::Shape<1, 1, 1, 16, 128>();
      pto::Stride<16384, 16384, 16384, 1024, 1> v28 = pto::Stride<16384, 16384, 16384, 1024, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v29 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v11 + v5 * (unsigned) v12), v27, v28);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v24, v29);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
      TASSIGN(v30, v15);
      Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
      __cbuf__ bfloat16_t* v32 = v30.data();
      uint64_t v33 = reinterpret_cast<uint64_t>(v32);
      TASSIGN(v31, v33);
      pto::Shape<1, 1, 1, 128, 64> v34 = pto::Shape<1, 1, 1, 128, 64>();
      pto::Stride<128, 128, 128, 1, 1024> v35 = pto::Stride<128, 128, 128, 1, 1024>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<128, 128, 128, 1, 1024>, pto::Layout::DN> v36 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<128, 128, 128, 1, 1024>, pto::Layout::DN>(v3 + (v5 + v5 * (unsigned) v12 + (unsigned) v22 * (unsigned) v11), v34, v35);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      TLOAD(v31, v36);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
      TASSIGN(v37, v16);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
      __ca__ bfloat16_t* v39 = v37.data();
      uint64_t v40 = reinterpret_cast<uint64_t>(v39);
      TASSIGN(v38, v40);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TMOV(v38, v24);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
      TASSIGN(v41, v16);
      Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
      __cb__ bfloat16_t* v43 = v41.data();
      uint64_t v44 = reinterpret_cast<uint64_t>(v43);
      TASSIGN(v42, v44);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      TMOV(v42, v31);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v7);
      TASSIGN(v45, v16);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v7);
      __cc__ float* v47 = v45.data();
      uint64_t v48 = reinterpret_cast<uint64_t>(v47);
      TASSIGN(v46, v48);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v46, v38, v42);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      for (size_t v49 = v17; v49 < v18; v49 += v17) {
        int32_t v50 = (int32_t) ((uint32_t) ((int32_t) v49) * (uint32_t) v6);
        Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
        TASSIGN(v51, v16);
        Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
        __cbuf__ bfloat16_t* v53 = v51.data();
        uint64_t v54 = reinterpret_cast<uint64_t>(v53);
        TASSIGN(v52, v54);
        pto::Shape<1, 1, 1, 16, 128> v55 = pto::Shape<1, 1, 1, 16, 128>();
        pto::Stride<16384, 16384, 16384, 1024, 1> v56 = pto::Stride<16384, 16384, 16384, 1024, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v57 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v11 + (unsigned) v50 * (unsigned) v12), v55, v56);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        TLOAD(v52, v57);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
        TASSIGN(v58, v15);
        Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Mat, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
        __cbuf__ bfloat16_t* v60 = v58.data();
        uint64_t v61 = reinterpret_cast<uint64_t>(v60);
        TASSIGN(v59, v61);
        pto::Shape<1, 1, 1, 128, 64> v62 = pto::Shape<1, 1, 1, 128, 64>();
        pto::Stride<128, 128, 128, 1, 1024> v63 = pto::Stride<128, 128, 128, 1, 1024>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<128, 128, 128, 1, 1024>, pto::Layout::DN> v64 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 64>, pto::Stride<128, 128, 128, 1, 1024>, pto::Layout::DN>(v3 + (v5 + (unsigned) v50 * (unsigned) v12 + (unsigned) v22 * (unsigned) v11), v62, v63);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        TLOAD(v59, v64);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
        TASSIGN(v65, v16);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v66 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v14, v6);
        __ca__ bfloat16_t* v67 = v65.data();
        uint64_t v68 = reinterpret_cast<uint64_t>(v67);
        TASSIGN(v66, v68);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
        TMOV(v66, v52);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
        TASSIGN(v69, v16);
        Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v70 = Tile<TileType::Right, bfloat16_t, 128, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v6, v7);
        __cb__ bfloat16_t* v71 = v69.data();
        uint64_t v72 = reinterpret_cast<uint64_t>(v71);
        TASSIGN(v70, v72);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        TMOV(v70, v59);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v7);
        TASSIGN(v73, v16);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v74 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v14, v7);
        __cc__ float* v75 = v73.data();
        uint64_t v76 = reinterpret_cast<uint64_t>(v75);
        TASSIGN(v74, v76);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        TMATMUL_ACC(v74, v74, v66, v70);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
      };
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      pto::Shape<1, 1, 1, 16, 64> v77 = pto::Shape<1, 1, 1, 16, 64>();
      pto::Stride<2433024, 2433024, 2433024, 152064, 1> v78 = pto::Stride<2433024, 2433024, 2433024, 152064, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<2433024, 2433024, 2433024, 152064, 1>, pto::Layout::ND> v79 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<2433024, 2433024, 2433024, 152064, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v13 + (unsigned) v22 * (unsigned) v12), v77, v78);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v79, v46);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v21 = v1;
    } else {
      v21 = v1;
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
