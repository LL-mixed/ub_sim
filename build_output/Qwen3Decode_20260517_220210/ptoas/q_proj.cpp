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

__global__ AICORE void q_proj(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ bfloat16_t* v3, int32_t v4, int32_t v5, int32_t v6) {
  unsigned v7 = 0;
  const int32_t v8 = 10;
  const int32_t v9 = 256;
  const int32_t v10 = 0;
  const int32_t v11 = 512;
  const int32_t v12 = 64;
  const int32_t v13 = 4;
  const int32_t v14 = 1;
  const int32_t v15 = 5120;
  const int32_t v16 = 16;
  const int64_t v17 = 32768;
  const int64_t v18 = 8192;
  const int64_t v19 = 16384;
  const int64_t v20 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v21 = (size_t) v14;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
  for (size_t v22 = (size_t) v4; v22 < ((size_t) ((int32_t) (uint32_t) v4 + (uint32_t) v13)); v22 += v21) {
    int32_t v23 = (int32_t) ((uint32_t) ((int32_t) v22) * (uint32_t) v12);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v11);
    TASSIGN(v24, v20);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v11);
    __cbuf__ bfloat16_t* v26 = v24.data();
    uint64_t v27 = reinterpret_cast<uint64_t>(v26);
    TASSIGN(v25, v27);
    pto::Shape<1, 1, 1, 16, 512> v28 = pto::Shape<1, 1, 1, 16, 512>();
    pto::Stride<81920, 81920, 81920, 5120, 1> v29 = pto::Stride<81920, 81920, 81920, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v30 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v2 + (v7 + v7 * (unsigned) v15 + v7 * (unsigned) v14), v28, v29);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    TLOAD(v25, v30);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    TASSIGN(v31, v19);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    __cbuf__ bfloat16_t* v33 = v31.data();
    uint64_t v34 = reinterpret_cast<uint64_t>(v33);
    TASSIGN(v32, v34);
    pto::Shape<1, 1, 1, 512, 64> v35 = pto::Shape<1, 1, 1, 512, 64>();
    pto::Stride<2621440, 2621440, 2621440, 5120, 1> v36 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v37 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v3 + (v7 + (unsigned) v5 * (unsigned) v15 + (unsigned) v23 * (unsigned) v14), v35, v36);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    TLOAD(v32, v37);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
    TASSIGN(v38, v20);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
    __ca__ bfloat16_t* v40 = v38.data();
    uint64_t v41 = reinterpret_cast<uint64_t>(v40);
    TASSIGN(v39, v41);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    TEXTRACT(v39, v25, v10, v10);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v42, v20);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v44 = v42.data();
    uint64_t v45 = reinterpret_cast<uint64_t>(v44);
    TASSIGN(v43, v45);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    TEXTRACT(v43, v32, v10, v10);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
    TASSIGN(v46, v18);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
    __ca__ bfloat16_t* v48 = v46.data();
    uint64_t v49 = reinterpret_cast<uint64_t>(v48);
    TASSIGN(v47, v49);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    TEXTRACT(v47, v25, v10, v9);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v50, v17);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v52 = v50.data();
    uint64_t v53 = reinterpret_cast<uint64_t>(v52);
    TASSIGN(v51, v53);
    TEXTRACT(v51, v32, v9, v10);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
    TASSIGN(v54, v20);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
    __cc__ float* v56 = v54.data();
    uint64_t v57 = reinterpret_cast<uint64_t>(v56);
    TASSIGN(v55, v57);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
    TMATMUL(v55, v39, v43);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
    TASSIGN(v58, v20);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
    __cc__ float* v60 = v58.data();
    uint64_t v61 = reinterpret_cast<uint64_t>(v60);
    TASSIGN(v59, v61);
    pipe_barrier(PIPE_M);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    TMATMUL_ACC(v59, v59, v47, v51);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
    for (size_t v62 = v21; v62 < ((size_t) v8); v62 += v21) {
      int32_t v63 = (int32_t) ((uint32_t) ((int32_t) v62) * (uint32_t) v11);
      Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v11);
      TASSIGN(v64, v20);
      Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v11);
      __cbuf__ bfloat16_t* v66 = v64.data();
      uint64_t v67 = reinterpret_cast<uint64_t>(v66);
      TASSIGN(v65, v67);
      pto::Shape<1, 1, 1, 16, 512> v68 = pto::Shape<1, 1, 1, 16, 512>();
      pto::Stride<81920, 81920, 81920, 5120, 1> v69 = pto::Stride<81920, 81920, 81920, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v70 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v2 + (v7 + v7 * (unsigned) v15 + (unsigned) v63 * (unsigned) v14), v68, v69);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
      TLOAD(v65, v70);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
      Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v71 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
      TASSIGN(v71, v19);
      Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
      __cbuf__ bfloat16_t* v73 = v71.data();
      uint64_t v74 = reinterpret_cast<uint64_t>(v73);
      TASSIGN(v72, v74);
      pto::Shape<1, 1, 1, 512, 64> v75 = pto::Shape<1, 1, 1, 512, 64>();
      pto::Stride<2621440, 2621440, 2621440, 5120, 1> v76 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v77 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v3 + (v7 + (unsigned) ((int32_t) (uint32_t) v5 + (uint32_t) v63) * (unsigned) v15 + (unsigned) v23 * (unsigned) v14), v75, v76);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
      TLOAD(v72, v77);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v78 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
      TASSIGN(v78, v20);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
      __ca__ bfloat16_t* v80 = v78.data();
      uint64_t v81 = reinterpret_cast<uint64_t>(v80);
      TASSIGN(v79, v81);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
      TEXTRACT(v79, v65, v10, v10);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v82 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
      TASSIGN(v82, v20);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v83 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
      __cb__ bfloat16_t* v84 = v82.data();
      uint64_t v85 = reinterpret_cast<uint64_t>(v84);
      TASSIGN(v83, v85);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
      TEXTRACT(v83, v72, v10, v10);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v86 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
      TASSIGN(v86, v18);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v87 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v16, v9);
      __ca__ bfloat16_t* v88 = v86.data();
      uint64_t v89 = reinterpret_cast<uint64_t>(v88);
      TASSIGN(v87, v89);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
      TEXTRACT(v87, v65, v10, v9);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v90 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
      TASSIGN(v90, v17);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v91 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
      __cb__ bfloat16_t* v92 = v90.data();
      uint64_t v93 = reinterpret_cast<uint64_t>(v92);
      TASSIGN(v91, v93);
      TEXTRACT(v91, v72, v9, v10);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v94 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
      TASSIGN(v94, v20);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v95 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
      __cc__ float* v96 = v94.data();
      uint64_t v97 = reinterpret_cast<uint64_t>(v96);
      TASSIGN(v95, v97);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
      pipe_barrier(PIPE_M);
      TMATMUL_ACC(v95, v95, v79, v83);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v98 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
      TASSIGN(v98, v20);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v99 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v16, v12);
      __cc__ float* v100 = v98.data();
      uint64_t v101 = reinterpret_cast<uint64_t>(v100);
      TASSIGN(v99, v101);
      pipe_barrier(PIPE_M);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
      TMATMUL_ACC(v99, v99, v87, v91);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
    };
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    pto::Shape<1, 1, 1, 16, 64> v102 = pto::Shape<1, 1, 1, 16, 64>();
    pto::Stride<81920, 81920, 81920, 5120, 1> v103 = pto::Stride<81920, 81920, 81920, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v104 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v1 + (v7 + (unsigned) v6 * (unsigned) v15 + (unsigned) v23 * (unsigned) v14), v102, v103);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(v104, v59);
    set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  }
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
