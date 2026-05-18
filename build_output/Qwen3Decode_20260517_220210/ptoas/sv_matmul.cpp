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

__global__ AICORE void sv_matmul(__gm__ float* v1, __gm__ int32_t* v2, __gm__ bfloat16_t* v3, __gm__ bfloat16_t* v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8, int32_t v9, int32_t v10) {
  unsigned v11 = 0;
  const int32_t v12 = 2;
  const int32_t v13 = 16;
  const int32_t v14 = 64;
  const int32_t v15 = 8;
  const int32_t v16 = 0;
  const int32_t v17 = 1;
  const int32_t v18 = 128;
  const int32_t v19 = 256;
  const int64_t v20 = 4096;
  const int64_t v21 = 32768;
  const int64_t v22 = 8192;
  const int64_t v23 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v24 = (size_t) v17;
  size_t v25 = (size_t) v16;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v26 = v25; v26 < ((size_t) v15); v26 += v24) {
    int32_t v27 = (int32_t) v26;
    for (size_t v28 = v25; v28 < ((size_t) v14); v28 += v24) {
      int32_t v29 = (int32_t) ((uint32_t) v5 + (uint32_t) ((int32_t) v28));
      __gm__ float* v30;
      if (v29 < v6) {
        int32_t v31 = v2[(int32_t) ((uint32_t) v7 + (uint32_t) v29)];
        Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v19);
        TASSIGN(v32, v23);
        Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v33 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v19);
        __cbuf__ bfloat16_t* v34 = v32.data();
        uint64_t v35 = reinterpret_cast<uint64_t>(v34);
        TASSIGN(v33, v35);
        int32_t v36 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v27 * (uint32_t) v12) + (uint32_t) v29) * (uint32_t) v13);
        pto::Shape<1, 1, 1, 16, 256> v37 = pto::Shape<1, 1, 1, 16, 256>();
        pto::Stride<4096, 4096, 4096, 256, 1> v38 = pto::Stride<4096, 4096, 4096, 256, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v39 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v3 + (v11 + (unsigned) v36 * (unsigned) v19 + v11 * (unsigned) v17), v37, v38);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
        TLOAD(v33, v39);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v19, v18);
        TASSIGN(v40, v22);
        Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v19, v18);
        __cbuf__ bfloat16_t* v42 = v40.data();
        uint64_t v43 = reinterpret_cast<uint64_t>(v42);
        TASSIGN(v41, v43);
        pto::Shape<1, 1, 1, 256, 128> v44 = pto::Shape<1, 1, 1, 256, 128>();
        pto::Stride<32768, 32768, 32768, 128, 1> v45 = pto::Stride<32768, 32768, 32768, 128, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND> v46 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND>(v4 + (v11 + (unsigned) ((int32_t) (uint32_t) v8 + (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v31 * (uint32_t) v15) + (uint32_t) v27) * (uint32_t) v19)) * (unsigned) v18 + v11 * (unsigned) v17), v44, v45);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
        TLOAD(v41, v46);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
        TASSIGN(v47, v23);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
        __ca__ bfloat16_t* v49 = v47.data();
        uint64_t v50 = reinterpret_cast<uint64_t>(v49);
        TASSIGN(v48, v50);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
        TEXTRACT(v48, v33, v16, v16);
        Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v18);
        TASSIGN(v51, v21);
        Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v18);
        __cb__ bfloat16_t* v53 = v51.data();
        uint64_t v54 = reinterpret_cast<uint64_t>(v53);
        TASSIGN(v52, v54);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
        TEXTRACT(v52, v41, v16, v16);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
        TASSIGN(v55, v20);
        Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
        __ca__ bfloat16_t* v57 = v55.data();
        uint64_t v58 = reinterpret_cast<uint64_t>(v57);
        TASSIGN(v56, v58);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
        TEXTRACT(v56, v33, v16, v18);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
        Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v18);
        TASSIGN(v59, v23);
        Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v18);
        __cb__ bfloat16_t* v61 = v59.data();
        uint64_t v62 = reinterpret_cast<uint64_t>(v61);
        TASSIGN(v60, v62);
        TEXTRACT(v60, v41, v18, v16);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v18);
        TASSIGN(v63, v23);
        Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v18);
        __cc__ float* v65 = v63.data();
        uint64_t v66 = reinterpret_cast<uint64_t>(v65);
        TASSIGN(v64, v66);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
        TMATMUL(v64, v48, v52);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
        Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v18);
        TASSIGN(v67, v23);
        Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v18);
        __cc__ float* v69 = v67.data();
        uint64_t v70 = reinterpret_cast<uint64_t>(v69);
        TASSIGN(v68, v70);
        pipe_barrier(PIPE_M);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        TMATMUL_ACC(v68, v68, v56, v60);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
        set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        pto::Shape<1, 1, 1, 16, 128> v71 = pto::Shape<1, 1, 1, 16, 128>();
        pto::Stride<2048, 2048, 2048, 128, 1> v72 = pto::Stride<2048, 2048, 2048, 128, 1>();
        GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v73 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v1 + (v11 + (unsigned) v36 * (unsigned) v18 + v11 * (unsigned) v17), v71, v72);
        wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        TSTORE(v73, v68);
        set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
        v30 = v1;
      } else {
        v30 = v1;
      };
    };
  }
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
