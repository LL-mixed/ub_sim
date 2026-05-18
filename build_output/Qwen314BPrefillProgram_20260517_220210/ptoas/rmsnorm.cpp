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

__global__ AICORE void rmsnorm(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4, int32_t v5, int32_t v6, int32_t v7) {
  RoundMode v8 = RoundMode::CAST_ROUND;
  unsigned v9 = 0;
  const int32_t v10 = 2621440;
  const float v11 = 9.99999997E-7f;
  const float v12 = 1.95312503E-4f;
  const int32_t v13 = 128;
  const int32_t v14 = 40;
  const int32_t v15 = 0;
  const float v16 = 0.0f;
  const int32_t v17 = 64;
  const int32_t v18 = 1;
  const int32_t v19 = 5120;
  const int64_t v20 = 256;
  const int64_t v21 = 0;
  const int64_t v22 = 82944;
  const int64_t v23 = 50176;
  const int64_t v24 = 17408;
  const int64_t v25 = 1024;
  const int64_t v26 = 768;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v27 = (size_t) v18;
  size_t v28 = (size_t) v15;
  size_t v29 = (size_t) v14;
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v30, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v32 = v30.data();
  uint64_t v33 = reinterpret_cast<uint64_t>(v32);
  TASSIGN(v31, v33);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  TEXPANDS(v31, v16);
  for (size_t v34 = v28; v34 < v29; v34 += v27) {
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v35, v25);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ bfloat16_t* v37 = v35.data();
    uint64_t v38 = reinterpret_cast<uint64_t>(v37);
    TASSIGN(v36, v38);
    unsigned v39 = (unsigned) v6;
    pto::Shape<1, 1, 1, -1, 128> v40 = pto::Shape<1, 1, 1, -1, 128>(v6);
    pto::Stride<2621440, 2621440, 2621440, 5120, 1> v41 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v42 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v1 + ((v9 + (unsigned) v4 * (unsigned) v10) + (unsigned) v5 * (unsigned) v19 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v34) * (uint32_t) v13) * (unsigned) v18), v40, v41);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v36, v42);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v43, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v45 = v43.data();
    uint64_t v46 = reinterpret_cast<uint64_t>(v45);
    TASSIGN(v44, v46);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TCVT(v44, v36, v8);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v47, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v49 = v47.data();
    uint64_t v50 = reinterpret_cast<uint64_t>(v49);
    TASSIGN(v48, v50);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v51, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v53 = v51.data();
    uint64_t v54 = reinterpret_cast<uint64_t>(v53);
    TASSIGN(v52, v54);
    pipe_barrier(PIPE_V);
    TMUL(v52, v48, v48);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v55, v23);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v57 = v55.data();
    uint64_t v58 = reinterpret_cast<uint64_t>(v57);
    TASSIGN(v56, v58);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v18);
    TASSIGN(v59, v22);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v18);
    __ubuf__ float* v61 = v59.data();
    uint64_t v62 = reinterpret_cast<uint64_t>(v61);
    TASSIGN(v60, v62);
    pipe_barrier(PIPE_V);
    TROWSUM(v60, v52, v56);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
    TASSIGN(v63, v22);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
    __ubuf__ float* v65 = v63.data();
    uint64_t v66 = reinterpret_cast<uint64_t>(v65);
    TASSIGN(v64, v66);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
    TASSIGN(v67, v26);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
    __ubuf__ float* v69 = v67.data();
    uint64_t v70 = reinterpret_cast<uint64_t>(v69);
    TASSIGN(v68, v70);
    pipe_barrier(PIPE_V);
    TADD(v68, v31, v64);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v71 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v71, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v73 = v71.data();
  uint64_t v74 = reinterpret_cast<uint64_t>(v73);
  TASSIGN(v72, v74);
  pipe_barrier(PIPE_V);
  TMULS(v72, v31, v12);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v75, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v77 = v75.data();
  uint64_t v78 = reinterpret_cast<uint64_t>(v77);
  TASSIGN(v76, v78);
  pipe_barrier(PIPE_V);
  TADDS(v76, v72, v11);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v79, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v81 = v79.data();
  uint64_t v82 = reinterpret_cast<uint64_t>(v81);
  TASSIGN(v80, v82);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v83 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v83, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v84 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v85 = v83.data();
  uint64_t v86 = reinterpret_cast<uint64_t>(v85);
  TASSIGN(v84, v86);
  pipe_barrier(PIPE_V);
  TSQRT(v84, v80);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v87 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v87, v26);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v89 = v87.data();
  uint64_t v90 = reinterpret_cast<uint64_t>(v89);
  TASSIGN(v88, v90);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v91 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  TASSIGN(v91, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v92 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v17);
  __ubuf__ float* v93 = v91.data();
  uint64_t v94 = reinterpret_cast<uint64_t>(v93);
  TASSIGN(v92, v94);
  pipe_barrier(PIPE_V);
  TRECIP(v92, v88);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v95 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v18);
  TASSIGN(v95, v21);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v18);
  __ubuf__ float* v97 = v95.data();
  uint64_t v98 = reinterpret_cast<uint64_t>(v97);
  TASSIGN(v96, v98);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v99 = v28; v99 < v29; v99 += v27) {
    int32_t v100 = (int32_t) ((uint32_t) ((int32_t) v99) * (uint32_t) v13);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v101 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v101, v25);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v102 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ bfloat16_t* v103 = v101.data();
    uint64_t v104 = reinterpret_cast<uint64_t>(v103);
    TASSIGN(v102, v104);
    unsigned v105 = (unsigned) v6;
    pto::Shape<1, 1, 1, -1, 128> v106 = pto::Shape<1, 1, 1, -1, 128>(v6);
    pto::Stride<2621440, 2621440, 2621440, 5120, 1> v107 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v108 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v1 + ((v9 + (unsigned) v4 * (unsigned) v10) + (unsigned) v5 * (unsigned) v19 + (unsigned) v100 * (unsigned) v18), v106, v107);
    wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
    TLOAD(v102, v108);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v109 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v109, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v110 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v111 = v109.data();
    uint64_t v112 = reinterpret_cast<uint64_t>(v111);
    TASSIGN(v110, v112);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    TCVT(v110, v102, v8);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v113 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v113, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v114 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v115 = v113.data();
    uint64_t v116 = reinterpret_cast<uint64_t>(v115);
    TASSIGN(v114, v116);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v117 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v13);
    TASSIGN(v117, v20);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v118 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v13);
    __ubuf__ float* v119 = v117.data();
    uint64_t v120 = reinterpret_cast<uint64_t>(v119);
    TASSIGN(v118, v120);
    pto::Shape<1, 1, 1, 1, 128> v121 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<5120, 5120, 5120, 5120, 1> v122 = pto::Stride<5120, 5120, 5120, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND> v123 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND>(v3 + (v9 + v9 * (unsigned) v19 + (unsigned) v100 * (unsigned) v18), v121, v122);
    TLOAD(v118, v123);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v124 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v124, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v125 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v126 = v124.data();
    uint64_t v127 = reinterpret_cast<uint64_t>(v126);
    TASSIGN(v125, v127);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v125, v114, v96);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v128 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v128, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v129 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ float* v130 = v128.data();
    uint64_t v131 = reinterpret_cast<uint64_t>(v130);
    TASSIGN(v129, v131);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v129, v125, v118);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v132 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    TASSIGN(v132, v25);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v133 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v17, v13);
    __ubuf__ bfloat16_t* v134 = v132.data();
    uint64_t v135 = reinterpret_cast<uint64_t>(v134);
    TASSIGN(v133, v135);
    pipe_barrier(PIPE_V);
    TCVT(v133, v129, v8);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 64, 128> v136 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v137 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v138 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v9 + v9 * (unsigned) v19 + (unsigned) v100 * (unsigned) v18), v136, v137);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v138, v133);
    set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
