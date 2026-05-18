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

__global__ AICORE void qk_matmul(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ int32_t* v3, __gm__ bfloat16_t* v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8, int32_t v9, int32_t v10, int32_t v11) {
  unsigned v12 = 0;
  const int32_t v13 = 2;
  const int32_t v14 = 64;
  const int32_t v15 = 16;
  const int32_t v16 = 8;
  const int32_t v17 = 0;
  const int32_t v18 = 128;
  const int32_t v19 = 1;
  const int32_t v20 = 256;
  const int64_t v21 = 2048;
  const int64_t v22 = 32768;
  const int64_t v23 = 4096;
  const int64_t v24 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v25 = (size_t) v19;
  size_t v26 = (size_t) v17;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v27 = v26; v27 < ((size_t) v16); v27 += v25) {
    int32_t v28 = (int32_t) v27;
    Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v18);
    TASSIGN(v29, v24);
    Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v18);
    __cbuf__ bfloat16_t* v31 = v29.data();
    uint64_t v32 = reinterpret_cast<uint64_t>(v31);
    TASSIGN(v30, v32);
    pto::Shape<1, 1, 1, 16, 128> v33 = pto::Shape<1, 1, 1, 16, 128>();
    pto::Stride<2048, 2048, 2048, 128, 1> v34 = pto::Stride<2048, 2048, 2048, 128, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v35 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v2 + (v12 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v5 * (uint32_t) v18) + (uint32_t) ((int32_t) (uint32_t) v28 * (uint32_t) v15)) * (unsigned) v18 + v12 * (unsigned) v19), v33, v34);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    pipe_barrier(PIPE_MTE2);
    TLOAD(v30, v35);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    for (size_t v36 = v26; v36 < ((size_t) v14); v36 += v25) {
      int32_t v37 = (int32_t) ((uint32_t) v6 + (uint32_t) ((int32_t) v36));
      __gm__ float* v38;
      if (v37 < v7) {
        int32_t v39 = v3[(int32_t) ((uint32_t) v8 + (uint32_t) v37)];
        Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
        TASSIGN(v40, v23);
        Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
        __cbuf__ bfloat16_t* v42 = v40.data();
        uint64_t v43 = reinterpret_cast<uint64_t>(v42);
        TASSIGN(v41, v43);
        pto::Shape<1, 1, 1, 128, 256> v44 = pto::Shape<1, 1, 1, 128, 256>();
        pto::Stride<128, 128, 128, 1, 128> v45 = pto::Stride<128, 128, 128, 1, 128>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<128, 128, 128, 1, 128>, pto::Layout::DN> v46 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<128, 128, 128, 1, 128>, pto::Layout::DN>(v4 + (v12 + v12 * (unsigned) v19 + (unsigned) ((int32_t) (uint32_t) v9 + (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v39 * (uint32_t) v16) + (uint32_t) v28) * (uint32_t) v20)) * (unsigned) v18), v44, v45);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
        TLOAD(v41, v46);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
        Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v14);
        TASSIGN(v47, v24);
        Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v14);
        __ca__ bfloat16_t* v49 = v47.data();
        uint64_t v50 = reinterpret_cast<uint64_t>(v49);
        TASSIGN(v48, v50);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
        TEXTRACT(v48, v30, v17, v17);
        Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v14, v20);
        TASSIGN(v51, v22);
        Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v14, v20);
        __cb__ bfloat16_t* v53 = v51.data();
        uint64_t v54 = reinterpret_cast<uint64_t>(v53);
        TASSIGN(v52, v54);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
        TEXTRACT(v52, v41, v17, v17);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v14);
        TASSIGN(v55, v21);
        Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v14);
        __ca__ bfloat16_t* v57 = v55.data();
        uint64_t v58 = reinterpret_cast<uint64_t>(v57);
        TASSIGN(v56, v58);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
        TEXTRACT(v56, v30, v17, v14);
        Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v14, v20);
        TASSIGN(v59, v24);
        Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v14, v20);
        __cb__ bfloat16_t* v61 = v59.data();
        uint64_t v62 = reinterpret_cast<uint64_t>(v61);
        TASSIGN(v60, v62);
        TEXTRACT(v60, v41, v14, v17);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
        TASSIGN(v63, v24);
        Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
        __cc__ float* v65 = v63.data();
        uint64_t v66 = reinterpret_cast<uint64_t>(v65);
        TASSIGN(v64, v66);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
        wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
        TMATMUL(v64, v48, v52);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
        Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
        TASSIGN(v67, v24);
        Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
        __cc__ float* v69 = v67.data();
        uint64_t v70 = reinterpret_cast<uint64_t>(v69);
        TASSIGN(v68, v70);
        pipe_barrier(PIPE_M);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
        TMATMUL_ACC(v68, v68, v56, v60);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
        set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        pto::Shape<1, 1, 1, 16, 256> v71 = pto::Shape<1, 1, 1, 16, 256>();
        pto::Stride<4096, 4096, 4096, 256, 1> v72 = pto::Stride<4096, 4096, 4096, 256, 1>();
        GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v73 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v1 + (v12 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v28 * (uint32_t) v13) + (uint32_t) v37) * (uint32_t) v15) * (unsigned) v20 + v12 * (unsigned) v19), v71, v72);
        wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
        TSTORE(v73, v68);
        set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
        v38 = v1;
      } else {
        v38 = v1;
      };
    };
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
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
