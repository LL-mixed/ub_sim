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

__global__ AICORE void decode_q_proj(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ bfloat16_t* v3, int32_t v4, int32_t v5, int32_t v6) {
  unsigned v7 = 0;
  const int32_t v8 = 10;
  const int32_t v9 = 256;
  const int32_t v10 = 512;
  const int32_t v11 = 64;
  const int32_t v12 = 80;
  const int32_t v13 = 4;
  const int32_t v14 = 0;
  const int32_t v15 = 1;
  const int32_t v16 = 5120;
  const int32_t v17 = 16;
  const int64_t v18 = 32768;
  const int64_t v19 = 8192;
  const int64_t v20 = 16384;
  const int64_t v21 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v22 = (size_t) v15;
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
  for (size_t v23 = (size_t) v14; v23 < ((size_t) v13); v23 += v22) {
    int32_t v24 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v4 * (uint32_t) v13) + (uint32_t) ((int32_t) v23));
    __gm__ float* v25;
    if (v24 < v12) {
      int32_t v26 = (int32_t) ((uint32_t) v24 * (uint32_t) v11);
      Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v27 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v10);
      TASSIGN(v27, v21);
      Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v28 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v10);
      __cbuf__ bfloat16_t* v29 = v27.data();
      uint64_t v30 = reinterpret_cast<uint64_t>(v29);
      TASSIGN(v28, v30);
      pto::Shape<1, 1, 1, 16, 512> v31 = pto::Shape<1, 1, 1, 16, 512>();
      pto::Stride<81920, 81920, 81920, 5120, 1> v32 = pto::Stride<81920, 81920, 81920, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v33 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v2 + (v7 + v7 * (unsigned) v16 + v7 * (unsigned) v15), v31, v32);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      TLOAD(v28, v33);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v34 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
      TASSIGN(v34, v20);
      Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
      __cbuf__ bfloat16_t* v36 = v34.data();
      uint64_t v37 = reinterpret_cast<uint64_t>(v36);
      TASSIGN(v35, v37);
      pto::Shape<1, 1, 1, 512, 64> v38 = pto::Shape<1, 1, 1, 512, 64>();
      pto::Stride<2621440, 2621440, 2621440, 5120, 1> v39 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v40 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v3 + (v7 + (unsigned) v5 * (unsigned) v16 + (unsigned) v26 * (unsigned) v15), v38, v39);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      TLOAD(v35, v40);
      set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
      TASSIGN(v41, v21);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
      __ca__ bfloat16_t* v43 = v41.data();
      uint64_t v44 = reinterpret_cast<uint64_t>(v43);
      TASSIGN(v42, v44);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      TEXTRACT(v42, v28, v14, v14);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
      TASSIGN(v45, v21);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
      __cb__ bfloat16_t* v47 = v45.data();
      uint64_t v48 = reinterpret_cast<uint64_t>(v47);
      TASSIGN(v46, v48);
      wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v46, v35, v14, v14);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
      TASSIGN(v49, v19);
      Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
      __ca__ bfloat16_t* v51 = v49.data();
      uint64_t v52 = reinterpret_cast<uint64_t>(v51);
      TASSIGN(v50, v52);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      TEXTRACT(v50, v28, v14, v9);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
      TASSIGN(v53, v18);
      Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
      __cb__ bfloat16_t* v55 = v53.data();
      uint64_t v56 = reinterpret_cast<uint64_t>(v55);
      TASSIGN(v54, v56);
      TEXTRACT(v54, v35, v9, v14);
      set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
      TASSIGN(v57, v21);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
      __cc__ float* v59 = v57.data();
      uint64_t v60 = reinterpret_cast<uint64_t>(v59);
      TASSIGN(v58, v60);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
      wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      TMATMUL(v58, v42, v46);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
      TASSIGN(v61, v21);
      Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
      __cc__ float* v63 = v61.data();
      uint64_t v64 = reinterpret_cast<uint64_t>(v63);
      TASSIGN(v62, v64);
      pipe_barrier(PIPE_M);
      wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
      TMATMUL_ACC(v62, v62, v50, v54);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
      wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
      wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
      for (size_t v65 = v22; v65 < ((size_t) v8); v65 += v22) {
        int32_t v66 = (int32_t) ((uint32_t) ((int32_t) v65) * (uint32_t) v10);
        Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v10);
        TASSIGN(v67, v21);
        Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v10);
        __cbuf__ bfloat16_t* v69 = v67.data();
        uint64_t v70 = reinterpret_cast<uint64_t>(v69);
        TASSIGN(v68, v70);
        pto::Shape<1, 1, 1, 16, 512> v71 = pto::Shape<1, 1, 1, 16, 512>();
        pto::Stride<81920, 81920, 81920, 5120, 1> v72 = pto::Stride<81920, 81920, 81920, 5120, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v73 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v2 + (v7 + v7 * (unsigned) v16 + (unsigned) v66 * (unsigned) v15), v71, v72);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        TLOAD(v68, v73);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v74 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
        TASSIGN(v74, v20);
        Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
        __cbuf__ bfloat16_t* v76 = v74.data();
        uint64_t v77 = reinterpret_cast<uint64_t>(v76);
        TASSIGN(v75, v77);
        pto::Shape<1, 1, 1, 512, 64> v78 = pto::Shape<1, 1, 1, 512, 64>();
        pto::Stride<2621440, 2621440, 2621440, 5120, 1> v79 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v80 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v3 + (v7 + (unsigned) ((int32_t) (uint32_t) v5 + (uint32_t) v66) * (unsigned) v16 + (unsigned) v26 * (unsigned) v15), v78, v79);
        wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        TLOAD(v75, v80);
        set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v81 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
        TASSIGN(v81, v21);
        Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v82 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
        __ca__ bfloat16_t* v83 = v81.data();
        uint64_t v84 = reinterpret_cast<uint64_t>(v83);
        TASSIGN(v82, v84);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
        TEXTRACT(v82, v68, v14, v14);
        Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v85 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
        TASSIGN(v85, v21);
        Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v86 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
        __cb__ bfloat16_t* v87 = v85.data();
        uint64_t v88 = reinterpret_cast<uint64_t>(v87);
        TASSIGN(v86, v88);
        wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
        TEXTRACT(v86, v75, v14, v14);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
        Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v89 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
        TASSIGN(v89, v19);
        Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v90 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
        __ca__ bfloat16_t* v91 = v89.data();
        uint64_t v92 = reinterpret_cast<uint64_t>(v91);
        TASSIGN(v90, v92);
        wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
        TEXTRACT(v90, v68, v14, v9);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID4);
        Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v93 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
        TASSIGN(v93, v18);
        Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v94 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v11);
        __cb__ bfloat16_t* v95 = v93.data();
        uint64_t v96 = reinterpret_cast<uint64_t>(v95);
        TASSIGN(v94, v96);
        TEXTRACT(v94, v75, v9, v14);
        set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID5);
        set_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v97 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
        TASSIGN(v97, v21);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v98 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
        __cc__ float* v99 = v97.data();
        uint64_t v100 = reinterpret_cast<uint64_t>(v99);
        TASSIGN(v98, v100);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
        pipe_barrier(PIPE_M);
        TMATMUL_ACC(v98, v98, v82, v86);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID4);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v101 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
        TASSIGN(v101, v21);
        Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v102 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v11);
        __cc__ float* v103 = v101.data();
        uint64_t v104 = reinterpret_cast<uint64_t>(v103);
        TASSIGN(v102, v104);
        pipe_barrier(PIPE_M);
        wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
        TMATMUL_ACC(v102, v102, v90, v94);
        set_flag(PIPE_M, PIPE_MTE1, EVENT_ID5);
      };
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
      set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
      set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
      pto::Shape<1, 1, 1, 16, 64> v105 = pto::Shape<1, 1, 1, 16, 64>();
      pto::Stride<81920, 81920, 81920, 5120, 1> v106 = pto::Stride<81920, 81920, 81920, 5120, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v107 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v1 + (v7 + (unsigned) v6 * (unsigned) v16 + (unsigned) v26 * (unsigned) v15), v105, v106);
      wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
      TSTORE(v107, v62);
      set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
      v25 = v1;
    } else {
      v25 = v1;
    };
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
