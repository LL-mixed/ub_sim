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

__global__ AICORE void prefill_sv_matmul(__gm__ float* v1, __gm__ int32_t* v2, __gm__ bfloat16_t* v3, __gm__ bfloat16_t* v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8, int32_t v9, int32_t v10, int32_t v11) {
  unsigned v12 = 0;
  const int32_t v13 = 16;
  const int32_t v14 = 8;
  const int32_t v15 = 2;
  const int32_t v16 = 64;
  const int32_t v17 = 0;
  const int32_t v18 = 256;
  const int32_t v19 = 1;
  const int32_t v20 = 128;
  const int64_t v21 = 4096;
  const int64_t v22 = 32768;
  const int64_t v23 = 8192;
  const int64_t v24 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v25 = (size_t) v17; v25 < ((size_t) v16); v25 += (size_t) v19) {
    int32_t v26 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v5 * (uint32_t) v16) + (uint32_t) ((int32_t) v25));
    __gm__ float* v27;
    if (v26 < v6) {
      int32_t v28 = v2[(int32_t) ((uint32_t) ((int32_t) (uint32_t) v7 * (uint32_t) v15) + (uint32_t) v26)];
      Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
      TASSIGN(v29, v24);
      Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Mat, bfloat16_t, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v18);
      __cbuf__ bfloat16_t* v31 = v29.data();
      uint64_t v32 = reinterpret_cast<uint64_t>(v31);
      TASSIGN(v30, v32);
      int32_t v33 = (int32_t) ((uint32_t) v26 * (uint32_t) v13);
      pto::Shape<1, 1, 1, 16, 256> v34 = pto::Shape<1, 1, 1, 16, 256>();
      pto::Stride<4096, 4096, 4096, 256, 1> v35 = pto::Stride<4096, 4096, 4096, 256, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v36 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v3 + (v12 + (unsigned) v33 * (unsigned) v18 + v12 * (unsigned) v19), v34, v35);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v30, v36);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
      TASSIGN(v37, v23);
      Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Mat, bfloat16_t, 256, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v18, v20);
      __cbuf__ bfloat16_t* v39 = v37.data();
      uint64_t v40 = reinterpret_cast<uint64_t>(v39);
      TASSIGN(v38, v40);
      pto::Shape<1, 1, 1, 256, 128> v41 = pto::Shape<1, 1, 1, 256, 128>();
      pto::Stride<32768, 32768, 32768, 128, 1> v42 = pto::Stride<32768, 32768, 32768, 128, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND> v43 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 256, 128>, pto::Stride<32768, 32768, 32768, 128, 1>, pto::Layout::ND>(v4 + (v12 + (unsigned) ((int32_t) (uint32_t) v9 + (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v28 * (uint32_t) v14) + (uint32_t) v8) * (uint32_t) v18)) * (unsigned) v20 + v12 * (unsigned) v19), v41, v42);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      TLOAD(v38, v43);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v20);
      TASSIGN(v44, v24);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v20);
      __ca__ bfloat16_t* v46 = v44.data();
      uint64_t v47 = reinterpret_cast<uint64_t>(v46);
      TASSIGN(v45, v47);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TEXTRACT(v45, v30, v17, v17);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      TASSIGN(v48, v22);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      __cb__ bfloat16_t* v50 = v48.data();
      uint64_t v51 = reinterpret_cast<uint64_t>(v50);
      TASSIGN(v49, v51);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v49, v38, v17, v17);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v20);
      TASSIGN(v52, v21);
      Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Left, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v13, v20);
      __ca__ bfloat16_t* v54 = v52.data();
      uint64_t v55 = reinterpret_cast<uint64_t>(v54);
      TASSIGN(v53, v55);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v53, v30, v17, v20);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      TASSIGN(v56, v24);
      Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Right, bfloat16_t, 128, 128, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v20, v20);
      __cb__ bfloat16_t* v58 = v56.data();
      uint64_t v59 = reinterpret_cast<uint64_t>(v58);
      TASSIGN(v57, v59);
      TEXTRACT(v57, v38, v20, v17);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v20);
      TASSIGN(v60, v24);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v20);
      __cc__ float* v62 = v60.data();
      uint64_t v63 = reinterpret_cast<uint64_t>(v62);
      TASSIGN(v61, v63);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v61, v45, v49);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v20);
      TASSIGN(v64, v24);
      Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Acc, float, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v13, v20);
      __cc__ float* v66 = v64.data();
      uint64_t v67 = reinterpret_cast<uint64_t>(v66);
      TASSIGN(v65, v67);
      pipe_barrier(PIPE_M);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      TMATMUL_ACC(v65, v65, v53, v57);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      pto::Shape<1, 1, 1, 16, 128> v68 = pto::Shape<1, 1, 1, 16, 128>();
      pto::Stride<2048, 2048, 2048, 128, 1> v69 = pto::Stride<2048, 2048, 2048, 128, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v70 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v1 + (v12 + (unsigned) v33 * (unsigned) v20 + v12 * (unsigned) v19), v68, v69);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v70, v65);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v27 = v1;
    } else {
      v27 = v1;
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
