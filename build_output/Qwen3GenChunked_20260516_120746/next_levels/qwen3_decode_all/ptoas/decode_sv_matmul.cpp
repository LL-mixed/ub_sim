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

__global__ AICORE void decode_sv_matmul(__gm__ float* v1, __gm__ int32_t* v2, __gm__ bfloat16_t* v3, __gm__ bfloat16_t* v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8, int32_t v9, int32_t v10, int32_t v11, int32_t v12) {
  unsigned v13 = 0;
  const int32_t v14 = 2;
  const int32_t v15 = 16;
  const int32_t v16 = 8;
  const int32_t v17 = 64;
  const int32_t v18 = 0;
  const int32_t v19 = 1;
  const int32_t v20 = 128;
  const int32_t v21 = 256;
  const int64_t v22 = 4096;
  const int64_t v23 = 32768;
  const int64_t v24 = 8192;
  const int64_t v25 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v26 = (size_t) v18; v26 < ((size_t) v17); v26 += (size_t) v19) {
    int32_t v27 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v5 * (uint32_t) v17) + (uint32_t) ((int32_t) v26));
    __gm__ float* v28;
    if (v27 < v6) {
      int32_t v29 = v2[(int32_t) ((uint32_t) v7 + (uint32_t) v27)];
      Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v21);
      TASSIGN(v30, v25);
      Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v21);
      __cbuf__ bfloat16_t* v32 = v30.data();
      uint64_t v33 = reinterpret_cast<uint64_t>(v32);
      TASSIGN(v31, v33);
      int32_t v34 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v9 * (uint32_t) v14) + (uint32_t) v27) * (uint32_t) v15);
      pto::Shape<1, 1, 1, 16, 256> v35 = pto::Shape<1, 1, 1, 16, 256>();
      pto::Stride<4096, 4096, 4096, 256, 1> v36 = pto::Stride<4096, 4096, 4096, 256, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v37 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v3 + (v13 + (unsigned) v34 * (unsigned) v21 + v13 * (unsigned) v19), v35, v36);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v31, v37);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v21, v20);
      TASSIGN(v38, v24);
      Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v21, v20);
      __cbuf__ bfloat16_t* v40 = v38.data();
      uint64_t v41 = reinterpret_cast<uint64_t>(v40);
      TASSIGN(v39, v41);
      pto::Shape<1, 1, 1, 256, 128> v42 = pto::Shape<1, 1, 1, 256, 128>();
      pto::Stride<32768, 32768, 32768, 128, 1> v43 = pto::Stride<32768, 32768, 32768, 128, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND> v44 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND>(v4 + (v13 + (unsigned) ((int32_t) (uint32_t) v10 + (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v29 * (uint32_t) v16) + (uint32_t) v8) * (uint32_t) v21)) * (unsigned) v20 + v13 * (unsigned) v19), v42, v43);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      TLOAD(v39, v44);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v45, v25);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      __ca__ bfloat16_t* v47 = v45.data();
      uint64_t v48 = reinterpret_cast<uint64_t>(v47);
      TASSIGN(v46, v48);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TEXTRACT(v46, v31, v18, v18);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      TASSIGN(v49, v23);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      __cb__ bfloat16_t* v51 = v49.data();
      uint64_t v52 = reinterpret_cast<uint64_t>(v51);
      TASSIGN(v50, v52);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v50, v39, v18, v18);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v53, v22);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v20);
      __ca__ bfloat16_t* v55 = v53.data();
      uint64_t v56 = reinterpret_cast<uint64_t>(v55);
      TASSIGN(v54, v56);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v54, v31, v18, v20);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      TASSIGN(v57, v25);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      __cb__ bfloat16_t* v59 = v57.data();
      uint64_t v60 = reinterpret_cast<uint64_t>(v59);
      TASSIGN(v58, v60);
      TEXTRACT(v58, v39, v20, v18);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v61, v25);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
      __cc__ float* v63 = v61.data();
      uint64_t v64 = reinterpret_cast<uint64_t>(v63);
      TASSIGN(v62, v64);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v62, v46, v50);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
      TASSIGN(v65, v25);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v66 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v20);
      __cc__ float* v67 = v65.data();
      uint64_t v68 = reinterpret_cast<uint64_t>(v67);
      TASSIGN(v66, v68);
      pipe_barrier(PIPE_M);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      TMATMUL_ACC(v66, v66, v54, v58);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      pto::Shape<1, 1, 1, 16, 128> v69 = pto::Shape<1, 1, 1, 16, 128>();
      pto::Stride<2048, 2048, 2048, 128, 1> v70 = pto::Stride<2048, 2048, 2048, 128, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v71 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v1 + (v13 + (unsigned) v34 * (unsigned) v20 + v13 * (unsigned) v19), v69, v70);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v71, v66);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v28 = v1;
    } else {
      v28 = v1;
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
