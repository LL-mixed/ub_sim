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
  unsigned v7 = 512;
  unsigned v8 = 0;
  const int32_t v9 = 256;
  const int32_t v10 = 0;
  const int32_t v11 = 512;
  const int32_t v12 = 64;
  const int32_t v13 = 4;
  const int32_t v14 = 1024;
  const int32_t v15 = 1;
  const int32_t v16 = 2048;
  const int32_t v17 = 16;
  const int64_t v18 = 32768;
  const int64_t v19 = 8192;
  const int64_t v20 = 16384;
  const int64_t v21 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
  for (size_t v22 = (size_t) v4; v22 < ((size_t) ((int32_t) (uint32_t) v4 + (uint32_t) v13)); v22 += (size_t) v15) {
    int32_t v23 = (int32_t) ((uint32_t) ((int32_t) v22) * (uint32_t) v12);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v11);
    TASSIGN(v24, v21);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v11);
    __cbuf__ bfloat16_t* v26 = v24.data();
    uint64_t v27 = reinterpret_cast<uint64_t>(v26);
    TASSIGN(v25, v27);
    pto::Shape<1, 1, 1, 16, 512> v28 = pto::Shape<1, 1, 1, 16, 512>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v29 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v30 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v8 + v8 * (unsigned) v14 + v8 * (unsigned) v15), v28, v29);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    TLOAD(v25, v30);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    TASSIGN(v31, v20);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    __cbuf__ bfloat16_t* v33 = v31.data();
    uint64_t v34 = reinterpret_cast<uint64_t>(v33);
    TASSIGN(v32, v34);
    pto::Shape<1, 1, 1, 512, 64> v35 = pto::Shape<1, 1, 1, 512, 64>();
    pto::Stride<1048576, 1048576, 1048576, 2048, 1> v36 = pto::Stride<1048576, 1048576, 1048576, 2048, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<1048576, 1048576, 1048576, 2048, 1>, pto::Layout::ND> v37 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<1048576, 1048576, 1048576, 2048, 1>, pto::Layout::ND>(v3 + (v8 + (unsigned) v5 * (unsigned) v16 + (unsigned) v23 * (unsigned) v15), v35, v36);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    TLOAD(v32, v37);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    TASSIGN(v38, v21);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    __ca__ bfloat16_t* v40 = v38.data();
    uint64_t v41 = reinterpret_cast<uint64_t>(v40);
    TASSIGN(v39, v41);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    TEXTRACT(v39, v25, v10, v10);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v42, v21);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v44 = v42.data();
    uint64_t v45 = reinterpret_cast<uint64_t>(v44);
    TASSIGN(v43, v45);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
    TEXTRACT(v43, v32, v10, v10);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    TASSIGN(v46, v19);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    __ca__ bfloat16_t* v48 = v46.data();
    uint64_t v49 = reinterpret_cast<uint64_t>(v48);
    TASSIGN(v47, v49);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    TEXTRACT(v47, v25, v10, v9);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v50, v18);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v52 = v50.data();
    uint64_t v53 = reinterpret_cast<uint64_t>(v52);
    TASSIGN(v51, v53);
    TEXTRACT(v51, v32, v9, v10);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    TASSIGN(v54, v21);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    __cc__ float* v56 = v54.data();
    uint64_t v57 = reinterpret_cast<uint64_t>(v56);
    TASSIGN(v55, v57);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
    wait_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
    TMATMUL(v55, v39, v43);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    TASSIGN(v58, v21);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    __cc__ float* v60 = v58.data();
    uint64_t v61 = reinterpret_cast<uint64_t>(v60);
    TASSIGN(v59, v61);
    pipe_barrier(PIPE_M);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
    TMATMUL_ACC(v59, v59, v47, v51);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v11);
    TASSIGN(v62, v21);
    Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Mat, bfloat16_t, 16, 512, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v11);
    __cbuf__ bfloat16_t* v64 = v62.data();
    uint64_t v65 = reinterpret_cast<uint64_t>(v64);
    TASSIGN(v63, v65);
    pto::Shape<1, 1, 1, 16, 512> v66 = pto::Shape<1, 1, 1, 16, 512>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v67 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v68 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v8 + v8 * (unsigned) v14 + v7 * (unsigned) v15), v66, v67);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    TLOAD(v63, v68);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    TASSIGN(v69, v20);
    Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v70 = Tile<TileType::Mat, bfloat16_t, 512, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v11, v12);
    __cbuf__ bfloat16_t* v71 = v69.data();
    uint64_t v72 = reinterpret_cast<uint64_t>(v71);
    TASSIGN(v70, v72);
    pto::Shape<1, 1, 1, 512, 64> v73 = pto::Shape<1, 1, 1, 512, 64>();
    pto::Stride<1048576, 1048576, 1048576, 2048, 1> v74 = pto::Stride<1048576, 1048576, 1048576, 2048, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<1048576, 1048576, 1048576, 2048, 1>, pto::Layout::ND> v75 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 512, 64>, pto::Stride<1048576, 1048576, 1048576, 2048, 1>, pto::Layout::ND>(v3 + (v8 + (unsigned) ((int32_t) (uint32_t) v5 + (uint32_t) v11) * (unsigned) v16 + (unsigned) v23 * (unsigned) v15), v73, v74);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    TLOAD(v70, v75);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    TASSIGN(v76, v21);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v77 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    __ca__ bfloat16_t* v78 = v76.data();
    uint64_t v79 = reinterpret_cast<uint64_t>(v78);
    TASSIGN(v77, v79);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    TEXTRACT(v77, v63, v10, v10);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v80, v21);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v81 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v82 = v80.data();
    uint64_t v83 = reinterpret_cast<uint64_t>(v82);
    TASSIGN(v81, v83);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    TEXTRACT(v81, v70, v10, v10);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v84 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    TASSIGN(v84, v19);
    Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v85 = Tile<TileType::Left, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v17, v9);
    __ca__ bfloat16_t* v86 = v84.data();
    uint64_t v87 = reinterpret_cast<uint64_t>(v86);
    TASSIGN(v85, v87);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
    TEXTRACT(v85, v63, v10, v9);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    TASSIGN(v88, v18);
    Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v89 = Tile<TileType::Right, bfloat16_t, 256, 64, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v9, v12);
    __cb__ bfloat16_t* v90 = v88.data();
    uint64_t v91 = reinterpret_cast<uint64_t>(v90);
    TASSIGN(v89, v91);
    TEXTRACT(v89, v70, v9, v10);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v92 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    TASSIGN(v92, v21);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v93 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    __cc__ float* v94 = v92.data();
    uint64_t v95 = reinterpret_cast<uint64_t>(v94);
    TASSIGN(v93, v95);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
    pipe_barrier(PIPE_M);
    TMATMUL_ACC(v93, v93, v77, v81);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    TASSIGN(v96, v21);
    Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v97 = Tile<TileType::Acc, float, 16, 64, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v17, v12);
    __cc__ float* v98 = v96.data();
    uint64_t v99 = reinterpret_cast<uint64_t>(v98);
    TASSIGN(v97, v99);
    pipe_barrier(PIPE_M);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
    TMATMUL_ACC(v97, v97, v85, v89);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
    set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    pto::Shape<1, 1, 1, 16, 64> v100 = pto::Shape<1, 1, 1, 16, 64>();
    pto::Stride<32768, 32768, 32768, 2048, 1> v101 = pto::Stride<32768, 32768, 32768, 2048, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<32768, 32768, 32768, 2048, 1>, pto::Layout::ND> v102 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 64>, pto::Stride<32768, 32768, 32768, 2048, 1>, pto::Layout::ND>(v1 + (v8 + (unsigned) v6 * (unsigned) v16 + (unsigned) v23 * (unsigned) v15), v100, v101);
    wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
    TSTORE(v102, v97);
    set_flag(PIPE_FIX, PIPE_M, EVENT_ID0);
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
