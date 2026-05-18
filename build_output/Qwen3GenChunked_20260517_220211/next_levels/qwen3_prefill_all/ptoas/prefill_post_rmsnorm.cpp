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

__global__ AICORE void prefill_post_rmsnorm(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4) {
  RoundMode v5 = RoundMode::CAST_ROUND;
  unsigned v6 = 0;
  const float v7 = 9.99999997E-7f;
  const float v8 = 1.95312503E-4f;
  const int32_t v9 = 128;
  const int32_t v10 = 0;
  const float v11 = 0.0f;
  const int32_t v12 = 40;
  const int32_t v13 = 1;
  const int32_t v14 = 5120;
  const int32_t v15 = 64;
  const int64_t v16 = 768;
  const int64_t v17 = 256;
  const int64_t v18 = 0;
  const int64_t v19 = 82944;
  const int64_t v20 = 50176;
  const int64_t v21 = 17408;
  const int64_t v22 = 17152;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v23 = (size_t) v13;
  size_t v24 = (size_t) v12;
  size_t v25 = (size_t) v10;
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v26 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v26, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v27 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v28 = v26.data();
  uint64_t v29 = reinterpret_cast<uint64_t>(v28);
  TASSIGN(v27, v29);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  TEXPANDS(v27, v11);
  for (size_t v30 = v25; v30 < v24; v30 += v23) {
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v31, v21);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v33 = v31.data();
    uint64_t v34 = reinterpret_cast<uint64_t>(v33);
    TASSIGN(v32, v34);
    pto::Shape<1, 1, 1, 64, 128> v35 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v36 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v37 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v14 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v30) * (uint32_t) v9) * (unsigned) v13), v35, v36);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v32, v37);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v38, v21);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v40 = v38.data();
    uint64_t v41 = reinterpret_cast<uint64_t>(v40);
    TASSIGN(v39, v41);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TMUL(v39, v32, v32);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v42, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v44 = v42.data();
    uint64_t v45 = reinterpret_cast<uint64_t>(v44);
    TASSIGN(v43, v45);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v13);
    TASSIGN(v46, v19);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v13);
    __ubuf__ float* v48 = v46.data();
    uint64_t v49 = reinterpret_cast<uint64_t>(v48);
    TASSIGN(v47, v49);
    pipe_barrier(PIPE_V);
    TROWSUM(v47, v39, v43);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
    TASSIGN(v50, v19);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
    __ubuf__ float* v52 = v50.data();
    uint64_t v53 = reinterpret_cast<uint64_t>(v52);
    TASSIGN(v51, v53);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
    TASSIGN(v54, v22);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
    __ubuf__ float* v56 = v54.data();
    uint64_t v57 = reinterpret_cast<uint64_t>(v56);
    TASSIGN(v55, v57);
    pipe_barrier(PIPE_V);
    TADD(v55, v27, v51);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v58, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v59 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v60 = v58.data();
  uint64_t v61 = reinterpret_cast<uint64_t>(v60);
  TASSIGN(v59, v61);
  pipe_barrier(PIPE_V);
  TMULS(v59, v27, v8);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v62, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v63 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v64 = v62.data();
  uint64_t v65 = reinterpret_cast<uint64_t>(v64);
  TASSIGN(v63, v65);
  pipe_barrier(PIPE_V);
  TADDS(v63, v59, v7);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v66 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v66, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v67 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v68 = v66.data();
  uint64_t v69 = reinterpret_cast<uint64_t>(v68);
  TASSIGN(v67, v69);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v70 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v70, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v71 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v72 = v70.data();
  uint64_t v73 = reinterpret_cast<uint64_t>(v72);
  TASSIGN(v71, v73);
  pipe_barrier(PIPE_V);
  TSQRT(v71, v67);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v74 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v74, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v76 = v74.data();
  uint64_t v77 = reinterpret_cast<uint64_t>(v76);
  TASSIGN(v75, v77);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v78 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  TASSIGN(v78, v18);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v15);
  __ubuf__ float* v80 = v78.data();
  uint64_t v81 = reinterpret_cast<uint64_t>(v80);
  TASSIGN(v79, v81);
  pipe_barrier(PIPE_V);
  TRECIP(v79, v75);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v82 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v13);
  TASSIGN(v82, v18);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v83 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v13);
  __ubuf__ float* v84 = v82.data();
  uint64_t v85 = reinterpret_cast<uint64_t>(v84);
  TASSIGN(v83, v85);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v86 = v25; v86 < v24; v86 += v23) {
    int32_t v87 = (int32_t) ((uint32_t) ((int32_t) v86) * (uint32_t) v9);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v88, v21);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v89 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v90 = v88.data();
    uint64_t v91 = reinterpret_cast<uint64_t>(v90);
    TASSIGN(v89, v91);
    pto::Shape<1, 1, 1, 64, 128> v92 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v93 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v94 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v1 + (v6 + v6 * (unsigned) v14 + (unsigned) v87 * (unsigned) v13), v92, v93);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    TLOAD(v89, v94);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v95 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v9);
    TASSIGN(v95, v17);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v9);
    __ubuf__ float* v97 = v95.data();
    uint64_t v98 = reinterpret_cast<uint64_t>(v97);
    TASSIGN(v96, v98);
    pto::Shape<1, 1, 1, 1, 128> v99 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<5120, 5120, 5120, 5120, 1> v100 = pto::Stride<5120, 5120, 5120, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND> v101 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND>(v3 + (v6 + (unsigned) v4 * (unsigned) v14 + (unsigned) v87 * (unsigned) v13), v99, v100);
    TLOAD(v96, v101);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v102 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v102, v21);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v103 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v104 = v102.data();
    uint64_t v105 = reinterpret_cast<uint64_t>(v104);
    TASSIGN(v103, v105);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v103, v89, v83);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v106 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v106, v21);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v107 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ float* v108 = v106.data();
    uint64_t v109 = reinterpret_cast<uint64_t>(v108);
    TASSIGN(v107, v109);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v107, v103, v96);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v110 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    TASSIGN(v110, v16);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v111 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v15, v9);
    __ubuf__ bfloat16_t* v112 = v110.data();
    uint64_t v113 = reinterpret_cast<uint64_t>(v112);
    TASSIGN(v111, v113);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v111, v107, v5);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 64, 128> v114 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v115 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v116 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v6 + v6 * (unsigned) v14 + (unsigned) v87 * (unsigned) v13), v114, v115);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v116, v111);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
