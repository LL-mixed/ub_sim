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

__global__ AICORE void rmsnorm(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4, int32_t v5, int32_t v6) {
  RoundMode v7 = RoundMode::CAST_ROUND;
  unsigned v8 = 1024;
  unsigned v9 = 0;
  const float v10 = 9.99999997E-7f;
  const float v11 = 9.765625E-4f;
  const int32_t v12 = 512;
  const int32_t v13 = 2;
  const int32_t v14 = 0;
  const float v15 = 0.0f;
  const int32_t v16 = 1;
  const int32_t v17 = 1024;
  const int32_t v18 = 16;
  const int64_t v19 = 2112;
  const int64_t v20 = 64;
  const int64_t v21 = 0;
  const int64_t v22 = 100480;
  const int64_t v23 = 67712;
  const int64_t v24 = 34944;
  const int64_t v25 = 18560;
  const int64_t v26 = 18496;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v27 = (size_t) v16;
  size_t v28 = (size_t) v14;
  size_t v29 = (size_t) v13;
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v30, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v32 = v30.data();
  uint64_t v33 = reinterpret_cast<uint64_t>(v32);
  TASSIGN(v31, v33);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID3);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  TEXPANDS(v31, v15);
  for (size_t v34 = v28; v34 < v29; v34 += v27) {
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v12);
    TASSIGN(v35, v25);
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v12);
    __ubuf__ bfloat16_t* v37 = v35.data();
    uint64_t v38 = reinterpret_cast<uint64_t>(v37);
    TASSIGN(v36, v38);
    unsigned v39 = (unsigned) v5 * v8;
    pto::Shape<1, 1, 1, -1, 512> v40 = pto::Shape<1, 1, 1, -1, 512>(v5);
    pto::Stride<-1, -1, -1, 1024, 1> v41 = pto::Stride<-1, -1, -1, 1024, 1>(v39, v39, v39);
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 512>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v42 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 512>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v1 + (v9 + (unsigned) v4 * (unsigned) v17 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v34) * (uint32_t) v12) * (unsigned) v16), v40, v41);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v36, v42);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v43, v24);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v45 = v43.data();
    uint64_t v46 = reinterpret_cast<uint64_t>(v45);
    TASSIGN(v44, v46);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TCVT(v44, v36, v7);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v47, v24);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v49 = v47.data();
    uint64_t v50 = reinterpret_cast<uint64_t>(v49);
    TASSIGN(v48, v50);
    pipe_barrier(PIPE_V);
    TMUL(v48, v44, v44);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v51, v23);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v53 = v51.data();
    uint64_t v54 = reinterpret_cast<uint64_t>(v53);
    TASSIGN(v52, v54);
    Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v16);
    TASSIGN(v55, v22);
    Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v16);
    __ubuf__ float* v57 = v55.data();
    uint64_t v58 = reinterpret_cast<uint64_t>(v57);
    TASSIGN(v56, v58);
    pipe_barrier(PIPE_V);
    TROWSUM(v56, v48, v52);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
    TASSIGN(v59, v22);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
    __ubuf__ float* v61 = v59.data();
    uint64_t v62 = reinterpret_cast<uint64_t>(v61);
    TASSIGN(v60, v62);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
    TASSIGN(v63, v26);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
    __ubuf__ float* v65 = v63.data();
    uint64_t v66 = reinterpret_cast<uint64_t>(v65);
    TASSIGN(v64, v66);
    pipe_barrier(PIPE_V);
    TADD(v64, v31, v60);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v67, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v69 = v67.data();
  uint64_t v70 = reinterpret_cast<uint64_t>(v69);
  TASSIGN(v68, v70);
  pipe_barrier(PIPE_V);
  TMULS(v68, v31, v11);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v71 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v71, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v73 = v71.data();
  uint64_t v74 = reinterpret_cast<uint64_t>(v73);
  TASSIGN(v72, v74);
  pipe_barrier(PIPE_V);
  TADDS(v72, v68, v10);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v75, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v77 = v75.data();
  uint64_t v78 = reinterpret_cast<uint64_t>(v77);
  TASSIGN(v76, v78);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v79, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v81 = v79.data();
  uint64_t v82 = reinterpret_cast<uint64_t>(v81);
  TASSIGN(v80, v82);
  pipe_barrier(PIPE_V);
  TSQRT(v80, v76);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v83 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v83, v26);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v84 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v85 = v83.data();
  uint64_t v86 = reinterpret_cast<uint64_t>(v85);
  TASSIGN(v84, v86);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v87 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  TASSIGN(v87, v21);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v18);
  __ubuf__ float* v89 = v87.data();
  uint64_t v90 = reinterpret_cast<uint64_t>(v89);
  TASSIGN(v88, v90);
  pipe_barrier(PIPE_V);
  TRECIP(v88, v84);
  Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v91 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v16);
  TASSIGN(v91, v21);
  Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v92 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v16);
  __ubuf__ float* v93 = v91.data();
  uint64_t v94 = reinterpret_cast<uint64_t>(v93);
  TASSIGN(v92, v94);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v95 = v28; v95 < v29; v95 += v27) {
    int32_t v96 = (int32_t) ((uint32_t) ((int32_t) v95) * (uint32_t) v12);
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v97 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v12);
    TASSIGN(v97, v25);
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v98 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v12);
    __ubuf__ bfloat16_t* v99 = v97.data();
    uint64_t v100 = reinterpret_cast<uint64_t>(v99);
    TASSIGN(v98, v100);
    unsigned v101 = (unsigned) v5 * v8;
    pto::Shape<1, 1, 1, -1, 512> v102 = pto::Shape<1, 1, 1, -1, 512>(v5);
    pto::Stride<-1, -1, -1, 1024, 1> v103 = pto::Stride<-1, -1, -1, 1024, 1>(v101, v101, v101);
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 512>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v104 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 512>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v1 + (v9 + (unsigned) v4 * (unsigned) v17 + (unsigned) v96 * (unsigned) v16), v102, v103);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    TLOAD(v98, v104);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v105 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v105, v24);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v106 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v107 = v105.data();
    uint64_t v108 = reinterpret_cast<uint64_t>(v107);
    TASSIGN(v106, v108);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    pipe_barrier(PIPE_V);
    TCVT(v106, v98, v7);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    Tile<TileType::Vec, float, 1, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v109 = Tile<TileType::Vec, float, 1, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v12);
    TASSIGN(v109, v20);
    Tile<TileType::Vec, float, 1, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v110 = Tile<TileType::Vec, float, 1, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v16, v12);
    __ubuf__ float* v111 = v109.data();
    uint64_t v112 = reinterpret_cast<uint64_t>(v111);
    TASSIGN(v110, v112);
    pto::Shape<1, 1, 1, 1, 512> v113 = pto::Shape<1, 1, 1, 1, 512>();
    pto::Stride<1024, 1024, 1024, 1024, 1> v114 = pto::Stride<1024, 1024, 1024, 1024, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 512>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND> v115 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 512>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND>(v3 + (v9 + (unsigned) v6 * (unsigned) v17 + (unsigned) v96 * (unsigned) v16), v113, v114);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID3);
    TLOAD(v110, v115);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v116 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v116, v24);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v117 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v118 = v116.data();
    uint64_t v119 = reinterpret_cast<uint64_t>(v118);
    TASSIGN(v117, v119);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v117, v106, v92);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v120 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v120, v24);
    Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v121 = Tile<TileType::Vec, float, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ float* v122 = v120.data();
    uint64_t v123 = reinterpret_cast<uint64_t>(v122);
    TASSIGN(v121, v123);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v121, v117, v110);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID3);
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v124 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    TASSIGN(v124, v19);
    Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v125 = Tile<TileType::Vec, bfloat16_t, 16, 512, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v12);
    __ubuf__ bfloat16_t* v126 = v124.data();
    uint64_t v127 = reinterpret_cast<uint64_t>(v126);
    TASSIGN(v125, v127);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v125, v121, v7);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 16, 512> v128 = pto::Shape<1, 1, 1, 16, 512>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v129 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v130 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 512>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v9 + v9 * (unsigned) v17 + (unsigned) v96 * (unsigned) v16), v128, v129);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v130, v125);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID3);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
