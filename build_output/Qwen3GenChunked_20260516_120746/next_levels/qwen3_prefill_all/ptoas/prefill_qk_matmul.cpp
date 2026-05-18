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

__global__ AICORE void prefill_qk_matmul(__gm__ float* v1, __gm__ int32_t* v2, __gm__ bfloat16_t* v3, __gm__ bfloat16_t* v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8, int32_t v9, int32_t v10, int32_t v11) {
  unsigned v12 = 0;
  const int32_t v13 = 8;
  const int32_t v14 = 2;
  const int32_t v15 = 64;
  const int32_t v16 = 0;
  const int32_t v17 = 16;
  const int32_t v18 = 128;
  const int32_t v19 = 1;
  const int32_t v20 = 256;
  const int64_t v21 = 2048;
  const int64_t v22 = 32768;
  const int64_t v23 = 65536;
  const int64_t v24 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v25 = (size_t) v16; v25 < ((size_t) v15); v25 += (size_t) v19) {
    int32_t v26 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v5 * (uint32_t) v15) + (uint32_t) ((int32_t) v25));
    __gm__ float* v27;
    if (v26 < v6) {
      int32_t v28 = v2[(int32_t) ((uint32_t) ((int32_t) (uint32_t) v7 * (uint32_t) v14) + (uint32_t) v26)];
      Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
      TASSIGN(v29, v24);
      Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
      __cbuf__ bfloat16_t* v31 = v29.data();
      uint64_t v32 = reinterpret_cast<uint64_t>(v31);
      TASSIGN(v30, v32);
      pto::Shape<1, 1, 1, 128, 256> v33 = pto::Shape<1, 1, 1, 128, 256>();
      pto::Stride<128, 128, 128, 1, 128> v34 = pto::Stride<128, 128, 128, 1, 128>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<128, 128, 128, 1, 128>, pto::Layout::DN> v35 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<128, 128, 128, 1, 128>, pto::Layout::DN>(v3 + (v12 + v12 * (unsigned) v19 + (unsigned) ((int32_t) (uint32_t) v9 + (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v28 * (uint32_t) v13) + (uint32_t) v8) * (uint32_t) v20)) * (unsigned) v18), v33, v34);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v30, v35);
      Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v18);
      TASSIGN(v36, v23);
      Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v18);
      __cbuf__ bfloat16_t* v38 = v36.data();
      uint64_t v39 = reinterpret_cast<uint64_t>(v38);
      TASSIGN(v37, v39);
      pto::Shape<1, 1, 1, 16, 128> v40 = pto::Shape<1, 1, 1, 16, 128>();
      pto::Stride<2048, 2048, 2048, 128, 1> v41 = pto::Stride<2048, 2048, 2048, 128, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v42 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v4 + (v12 + v12 * (unsigned) v18 + v12 * (unsigned) v19), v40, v41);
      TLOAD(v37, v42);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v15);
      TASSIGN(v43, v24);
      Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v15);
      __ca__ bfloat16_t* v45 = v43.data();
      uint64_t v46 = reinterpret_cast<uint64_t>(v45);
      TASSIGN(v44, v46);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TEXTRACT(v44, v37, v16, v16);
      Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v47, v22);
      Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      __cb__ bfloat16_t* v49 = v47.data();
      uint64_t v50 = reinterpret_cast<uint64_t>(v49);
      TASSIGN(v48, v50);
      TEXTRACT(v48, v30, v16, v16);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v15);
      TASSIGN(v51, v21);
      Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v15);
      __ca__ bfloat16_t* v53 = v51.data();
      uint64_t v54 = reinterpret_cast<uint64_t>(v53);
      TASSIGN(v52, v54);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v52, v37, v16, v15);
      Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v55, v24);
      Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      __cb__ bfloat16_t* v57 = v55.data();
      uint64_t v58 = reinterpret_cast<uint64_t>(v57);
      TASSIGN(v56, v58);
      TEXTRACT(v56, v30, v15, v16);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v20);
      TASSIGN(v59, v24);
      Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v20);
      __cc__ float* v61 = v59.data();
      uint64_t v62 = reinterpret_cast<uint64_t>(v61);
      TASSIGN(v60, v62);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v60, v44, v48);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v20);
      TASSIGN(v63, v24);
      Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v20);
      __cc__ float* v65 = v63.data();
      uint64_t v66 = reinterpret_cast<uint64_t>(v65);
      TASSIGN(v64, v66);
      pipe_barrier(PIPE_M);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      TMATMUL_ACC(v64, v64, v52, v56);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      pto::Shape<1, 1, 1, 16, 256> v67 = pto::Shape<1, 1, 1, 16, 256>();
      pto::Stride<4096, 4096, 4096, 256, 1> v68 = pto::Stride<4096, 4096, 4096, 256, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v69 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v1 + (v12 + (unsigned) ((int32_t) (uint32_t) v26 * (uint32_t) v17) * (unsigned) v20 + v12 * (unsigned) v19), v67, v68);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v69, v64);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v27 = v1;
    } else {
      v27 = v1;
    };
  }
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
