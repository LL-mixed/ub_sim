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

__global__ AICORE void up_proj(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4, int32_t v5) {
  unsigned v6 = 0;
  const int32_t v7 = 8;
  const int32_t v8 = 64;
  const int32_t v9 = 0;
  const int32_t v10 = 128;
  const int32_t v11 = 256;
  const int32_t v12 = 3072;
  const int32_t v13 = 1;
  const int32_t v14 = 1024;
  const int32_t v15 = 16;
  const int64_t v16 = 32768;
  const int64_t v17 = 2048;
  const int64_t v18 = 4096;
  const int64_t v19 = 0;
  using T = float;

  #if defined(__DAV_CUBE__)
  size_t v20 = (size_t) v13;
  Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v21 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v10);
  TASSIGN(v21, v19);
  Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v22 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v10);
  __cbuf__ bfloat16_t* v23 = v21.data();
  uint64_t v24 = reinterpret_cast<uint64_t>(v23);
  TASSIGN(v22, v24);
  pto::Shape<1, 1, 1, 16, 128> v25 = pto::Shape<1, 1, 1, 16, 128>();
  pto::Stride<16384, 16384, 16384, 1024, 1> v26 = pto::Stride<16384, 16384, 16384, 1024, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v27 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v14 + v6 * (unsigned) v13), v25, v26);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
  TLOAD(v22, v27);
  set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
  Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v28 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
  TASSIGN(v28, v18);
  Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
  __cbuf__ bfloat16_t* v30 = v28.data();
  uint64_t v31 = reinterpret_cast<uint64_t>(v30);
  TASSIGN(v29, v31);
  pto::Shape<1, 1, 1, 128, 256> v32 = pto::Shape<1, 1, 1, 128, 256>();
  pto::Stride<393216, 393216, 393216, 3072, 1> v33 = pto::Stride<393216, 393216, 393216, 3072, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<393216, 393216, 393216, 3072, 1>, pto::Layout::ND> v34 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<393216, 393216, 393216, 3072, 1>, pto::Layout::ND>(v2 + (v6 + (unsigned) v4 * (unsigned) v12 + (unsigned) v5 * (unsigned) v13), v32, v33);
  TLOAD(v29, v34);
  set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
  Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
  TASSIGN(v35, v19);
  Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
  __ca__ bfloat16_t* v37 = v35.data();
  uint64_t v38 = reinterpret_cast<uint64_t>(v37);
  TASSIGN(v36, v38);
  wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID0);
  TEXTRACT(v36, v22, v9, v9);
  Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
  TASSIGN(v39, v19);
  Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
  __cb__ bfloat16_t* v41 = v39.data();
  uint64_t v42 = reinterpret_cast<uint64_t>(v41);
  TASSIGN(v40, v42);
  wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID1);
  TEXTRACT(v40, v29, v9, v9);
  set_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
  Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
  TASSIGN(v43, v17);
  Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
  __ca__ bfloat16_t* v45 = v43.data();
  uint64_t v46 = reinterpret_cast<uint64_t>(v45);
  TASSIGN(v44, v46);
  TEXTRACT(v44, v22, v9, v8);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
  TASSIGN(v47, v16);
  Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
  __cb__ bfloat16_t* v49 = v47.data();
  uint64_t v50 = reinterpret_cast<uint64_t>(v49);
  TASSIGN(v48, v50);
  TEXTRACT(v48, v29, v8, v9);
  set_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
  set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
  TASSIGN(v51, v19);
  Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
  __cc__ float* v53 = v51.data();
  uint64_t v54 = reinterpret_cast<uint64_t>(v53);
  TASSIGN(v52, v54);
  wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID0);
  TMATMUL(v52, v36, v40);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
  TASSIGN(v55, v19);
  Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
  __cc__ float* v57 = v55.data();
  uint64_t v58 = reinterpret_cast<uint64_t>(v57);
  TASSIGN(v56, v58);
  pipe_barrier(PIPE_M);
  wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID1);
  TMATMUL_ACC(v56, v56, v44, v48);
  set_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID1);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID0);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID1);
  for (size_t v59 = v20; v59 < ((size_t) v7); v59 += v20) {
    int32_t v60 = (int32_t) ((uint32_t) ((int32_t) v59) * (uint32_t) v10);
    Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v10);
    TASSIGN(v61, v19);
    Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Mat, bfloat16_t, 16, 128, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v10);
    __cbuf__ bfloat16_t* v63 = v61.data();
    uint64_t v64 = reinterpret_cast<uint64_t>(v63);
    TASSIGN(v62, v64);
    pto::Shape<1, 1, 1, 16, 128> v65 = pto::Shape<1, 1, 1, 16, 128>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v66 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v67 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v14 + (unsigned) v60 * (unsigned) v13), v65, v66);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    TLOAD(v62, v67);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
    TASSIGN(v68, v18);
    Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Mat, bfloat16_t, 128, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v10, v11);
    __cbuf__ bfloat16_t* v70 = v68.data();
    uint64_t v71 = reinterpret_cast<uint64_t>(v70);
    TASSIGN(v69, v71);
    pto::Shape<1, 1, 1, 128, 256> v72 = pto::Shape<1, 1, 1, 128, 256>();
    pto::Stride<393216, 393216, 393216, 3072, 1> v73 = pto::Stride<393216, 393216, 393216, 3072, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<393216, 393216, 393216, 3072, 1>, pto::Layout::ND> v74 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 128, 256>, pto::Stride<393216, 393216, 393216, 3072, 1>, pto::Layout::ND>(v2 + (v6 + (unsigned) ((int32_t) (uint32_t) v4 + (uint32_t) v60) * (unsigned) v12 + (unsigned) v5 * (unsigned) v13), v72, v73);
    wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    TLOAD(v69, v74);
    set_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
    TASSIGN(v75, v19);
    Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
    __ca__ bfloat16_t* v77 = v75.data();
    uint64_t v78 = reinterpret_cast<uint64_t>(v77);
    TASSIGN(v76, v78);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID2);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    TEXTRACT(v76, v62, v9, v9);
    Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    TASSIGN(v79, v19);
    Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    __cb__ bfloat16_t* v81 = v79.data();
    uint64_t v82 = reinterpret_cast<uint64_t>(v81);
    TASSIGN(v80, v82);
    wait_flag(PIPE_MTE2, PIPE_MTE1, EVENT_ID3);
    TEXTRACT(v80, v69, v9, v9);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
    Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v83 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
    TASSIGN(v83, v17);
    Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null> v84 = Tile<TileType::Left, bfloat16_t, 16, 64, BLayout::RowMajor, -1, -1, SLayout::RowMajor, 512, PadValue::Null, CompactMode::Null>(v15, v8);
    __ca__ bfloat16_t* v85 = v83.data();
    uint64_t v86 = reinterpret_cast<uint64_t>(v85);
    TASSIGN(v84, v86);
    wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
    TEXTRACT(v84, v62, v9, v8);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v87 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    TASSIGN(v87, v16);
    Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Right, bfloat16_t, 64, 256, BLayout::RowMajor, -1, -1, SLayout::ColMajor, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    __cb__ bfloat16_t* v89 = v87.data();
    uint64_t v90 = reinterpret_cast<uint64_t>(v89);
    TASSIGN(v88, v90);
    TEXTRACT(v88, v69, v8, v9);
    set_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
    set_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
    Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v91 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
    TASSIGN(v91, v19);
    Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v92 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
    __cc__ float* v93 = v91.data();
    uint64_t v94 = reinterpret_cast<uint64_t>(v93);
    TASSIGN(v92, v94);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID2);
    pipe_barrier(PIPE_M);
    TMATMUL_ACC(v92, v92, v76, v80);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
    Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v95 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
    TASSIGN(v95, v19);
    Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Acc, float, 16, 256, BLayout::ColMajor, -1, -1, SLayout::RowMajor, 1024, PadValue::Null, CompactMode::Null>(v15, v11);
    __cc__ float* v97 = v95.data();
    uint64_t v98 = reinterpret_cast<uint64_t>(v97);
    TASSIGN(v96, v98);
    pipe_barrier(PIPE_M);
    wait_flag(PIPE_MTE1, PIPE_M, EVENT_ID3);
    TMATMUL_ACC(v96, v96, v84, v88);
    set_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
  }
  set_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
  pto::Shape<1, 1, 1, 16, 256> v99 = pto::Shape<1, 1, 1, 16, 256>();
  pto::Stride<4096, 4096, 4096, 256, 1> v100 = pto::Stride<4096, 4096, 4096, 256, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v101 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v3 + (v6 + v6 * (unsigned) v11 + v6 * (unsigned) v13), v99, v100);
  wait_flag(PIPE_M, PIPE_FIX, EVENT_ID0);
  TSTORE(v101, v56);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID2);
  wait_flag(PIPE_MTE1, PIPE_MTE2, EVENT_ID3);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID2);
  wait_flag(PIPE_M, PIPE_MTE1, EVENT_ID3);
  #endif // __DAV_CUBE__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
