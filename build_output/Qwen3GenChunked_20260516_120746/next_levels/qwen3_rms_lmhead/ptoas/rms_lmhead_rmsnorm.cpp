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

__global__ AICORE void rms_lmhead_rmsnorm(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3) {
  RoundMode v4 = RoundMode::CAST_ROUND;
  unsigned v5 = 0;
  const float v6 = 9.99999997E-7f;
  const float v7 = 9.765625E-4f;
  const int32_t v8 = 128;
  const int32_t v9 = 8;
  const int32_t v10 = 0;
  const float v11 = 0.0f;
  const int32_t v12 = 1;
  const int32_t v13 = 1024;
  const int32_t v14 = 16;
  const int64_t v15 = 0;
  const int64_t v16 = 21056;
  const int64_t v17 = 12864;
  const int64_t v18 = 4672;
  const int64_t v19 = 576;
  const int64_t v20 = 512;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v21 = (size_t) v12;
  size_t v22 = (size_t) v10;
  size_t v23 = (size_t) v9;
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v24, v20);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v26 = v24.data();
  uint64_t v27 = reinterpret_cast<uint64_t>(v26);
  TASSIGN(v25, v27);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  TEXPANDS(v25, v11);
  for (size_t v28 = v22; v28 < v23; v28 += v21) {
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v29, v19);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ bfloat16_t* v31 = v29.data();
    uint64_t v32 = reinterpret_cast<uint64_t>(v31);
    TASSIGN(v30, v32);
    pto::Shape<1, 1, 1, 16, 128> v33 = pto::Shape<1, 1, 1, 16, 128>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v34 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v35 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v13 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v28) * (uint32_t) v8) * (unsigned) v12), v33, v34);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v30, v35);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v36, v18);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v38 = v36.data();
    uint64_t v39 = reinterpret_cast<uint64_t>(v38);
    TASSIGN(v37, v39);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TCVT(v37, v30, v4);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v40 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v40, v18);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v42 = v40.data();
    uint64_t v43 = reinterpret_cast<uint64_t>(v42);
    TASSIGN(v41, v43);
    pipe_barrier(PIPE_V);
    TMUL(v41, v37, v37);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v44, v17);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v46 = v44.data();
    uint64_t v47 = reinterpret_cast<uint64_t>(v46);
    TASSIGN(v45, v47);
    Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
    TASSIGN(v48, v16);
    Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
    __ubuf__ float* v50 = v48.data();
    uint64_t v51 = reinterpret_cast<uint64_t>(v50);
    TASSIGN(v49, v51);
    pipe_barrier(PIPE_V);
    TROWSUM(v49, v41, v45);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    TASSIGN(v52, v16);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    __ubuf__ float* v54 = v52.data();
    uint64_t v55 = reinterpret_cast<uint64_t>(v54);
    TASSIGN(v53, v55);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    TASSIGN(v56, v20);
    Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    __ubuf__ float* v58 = v56.data();
    uint64_t v59 = reinterpret_cast<uint64_t>(v58);
    TASSIGN(v57, v59);
    pipe_barrier(PIPE_V);
    TADD(v57, v25, v53);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v60, v20);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v62 = v60.data();
  uint64_t v63 = reinterpret_cast<uint64_t>(v62);
  TASSIGN(v61, v63);
  pipe_barrier(PIPE_V);
  TMULS(v61, v25, v7);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v64, v20);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v66 = v64.data();
  uint64_t v67 = reinterpret_cast<uint64_t>(v66);
  TASSIGN(v65, v67);
  pipe_barrier(PIPE_V);
  TADDS(v65, v61, v6);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v68, v20);
  Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Vec, float, 1, 16, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v70 = v68.data();
  uint64_t v71 = reinterpret_cast<uint64_t>(v70);
  TASSIGN(v69, v71);
  pipe_barrier(PIPE_V);
  TRSQRT(v69, v65);
  Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
  TASSIGN(v72, v20);
  Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
  __ubuf__ float* v74 = v72.data();
  uint64_t v75 = reinterpret_cast<uint64_t>(v74);
  TASSIGN(v73, v75);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v76 = v22; v76 < v23; v76 += v21) {
    int32_t v77 = (int32_t) ((uint32_t) ((int32_t) v76) * (uint32_t) v8);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v78 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v78, v19);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ bfloat16_t* v80 = v78.data();
    uint64_t v81 = reinterpret_cast<uint64_t>(v80);
    TASSIGN(v79, v81);
    pto::Shape<1, 1, 1, 16, 128> v82 = pto::Shape<1, 1, 1, 16, 128>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v83 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v84 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v13 + (unsigned) v77 * (unsigned) v12), v82, v83);
    wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
    TLOAD(v79, v84);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v85 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v85, v18);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v86 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v87 = v85.data();
    uint64_t v88 = reinterpret_cast<uint64_t>(v87);
    TASSIGN(v86, v88);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    TCVT(v86, v79, v4);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v89 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    TASSIGN(v89, v15);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v90 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    __ubuf__ float* v91 = v89.data();
    uint64_t v92 = reinterpret_cast<uint64_t>(v91);
    TASSIGN(v90, v92);
    pto::Shape<1, 1, 1, 1, 128> v93 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<1024, 1024, 1024, 1024, 1> v94 = pto::Stride<1024, 1024, 1024, 1024, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND> v95 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND>(v3 + (v5 + v5 * (unsigned) v13 + (unsigned) v77 * (unsigned) v12), v93, v94);
    TLOAD(v90, v95);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v96, v18);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v97 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v98 = v96.data();
    uint64_t v99 = reinterpret_cast<uint64_t>(v98);
    TASSIGN(v97, v99);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v97, v86, v73);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v100 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v100, v18);
    Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v101 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v102 = v100.data();
    uint64_t v103 = reinterpret_cast<uint64_t>(v102);
    TASSIGN(v101, v103);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v101, v97, v90);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v104 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v104, v19);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v105 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ bfloat16_t* v106 = v104.data();
    uint64_t v107 = reinterpret_cast<uint64_t>(v106);
    TASSIGN(v105, v107);
    pipe_barrier(PIPE_V);
    TCVT(v105, v101, v4);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 16, 128> v108 = pto::Shape<1, 1, 1, 16, 128>();
    pto::Stride<16384, 16384, 16384, 1024, 1> v109 = pto::Stride<16384, 16384, 16384, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND> v110 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<16384, 16384, 16384, 1024, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v13 + (unsigned) v77 * (unsigned) v12), v108, v109);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v110, v105);
    set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
