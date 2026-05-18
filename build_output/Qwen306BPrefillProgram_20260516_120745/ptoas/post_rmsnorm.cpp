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

__global__ AICORE void post_rmsnorm(__gm__ float* v1, __gm__ bfloat16_t* v2, __gm__ float* v3) {
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
  const int32_t v14 = 64;
  const int64_t v15 = 768;
  const int64_t v16 = 256;
  const int64_t v17 = 0;
  const int64_t v18 = 82944;
  const int64_t v19 = 50176;
  const int64_t v20 = 17408;
  const int64_t v21 = 17152;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v22 = (size_t) v12;
  size_t v23 = (size_t) v10;
  size_t v24 = (size_t) v9;
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v25, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v26 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v27 = v25.data();
  uint64_t v28 = reinterpret_cast<uint64_t>(v27);
  TASSIGN(v26, v28);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  TEXPANDS(v26, v11);
  for (size_t v29 = v23; v29 < v24; v29 += v22) {
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v30, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v32 = v30.data();
    uint64_t v33 = reinterpret_cast<uint64_t>(v32);
    TASSIGN(v31, v33);
    pto::Shape<1, 1, 1, 64, 128> v34 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<65536, 65536, 65536, 1024, 1> v35 = pto::Stride<65536, 65536, 65536, 1024, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND> v36 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v13 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v29) * (uint32_t) v8) * (unsigned) v12), v34, v35);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v31, v36);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v37, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v39 = v37.data();
    uint64_t v40 = reinterpret_cast<uint64_t>(v39);
    TASSIGN(v38, v40);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TMUL(v38, v31, v31);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v41 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v41, v19);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v43 = v41.data();
    uint64_t v44 = reinterpret_cast<uint64_t>(v43);
    TASSIGN(v42, v44);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
    TASSIGN(v45, v18);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
    __ubuf__ float* v47 = v45.data();
    uint64_t v48 = reinterpret_cast<uint64_t>(v47);
    TASSIGN(v46, v48);
    pipe_barrier(PIPE_V);
    TROWSUM(v46, v38, v42);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    TASSIGN(v49, v18);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    __ubuf__ float* v51 = v49.data();
    uint64_t v52 = reinterpret_cast<uint64_t>(v51);
    TASSIGN(v50, v52);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    TASSIGN(v53, v21);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
    __ubuf__ float* v55 = v53.data();
    uint64_t v56 = reinterpret_cast<uint64_t>(v55);
    TASSIGN(v54, v56);
    pipe_barrier(PIPE_V);
    TADD(v54, v26, v50);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v57, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v58 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v59 = v57.data();
  uint64_t v60 = reinterpret_cast<uint64_t>(v59);
  TASSIGN(v58, v60);
  pipe_barrier(PIPE_V);
  TMULS(v58, v26, v7);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v61, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v62 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v63 = v61.data();
  uint64_t v64 = reinterpret_cast<uint64_t>(v63);
  TASSIGN(v62, v64);
  pipe_barrier(PIPE_V);
  TADDS(v62, v58, v6);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v65, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v66 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v67 = v65.data();
  uint64_t v68 = reinterpret_cast<uint64_t>(v67);
  TASSIGN(v66, v68);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v69, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v70 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v71 = v69.data();
  uint64_t v72 = reinterpret_cast<uint64_t>(v71);
  TASSIGN(v70, v72);
  pipe_barrier(PIPE_V);
  TSQRT(v70, v66);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v73, v21);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v74 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v75 = v73.data();
  uint64_t v76 = reinterpret_cast<uint64_t>(v75);
  TASSIGN(v74, v76);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v77 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  TASSIGN(v77, v17);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v78 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v14);
  __ubuf__ float* v79 = v77.data();
  uint64_t v80 = reinterpret_cast<uint64_t>(v79);
  TASSIGN(v78, v80);
  pipe_barrier(PIPE_V);
  TRECIP(v78, v74);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v81 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
  TASSIGN(v81, v17);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v82 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v12);
  __ubuf__ float* v83 = v81.data();
  uint64_t v84 = reinterpret_cast<uint64_t>(v83);
  TASSIGN(v82, v84);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v85 = v23; v85 < v24; v85 += v22) {
    int32_t v86 = (int32_t) ((uint32_t) ((int32_t) v85) * (uint32_t) v8);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v87 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v87, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v89 = v87.data();
    uint64_t v90 = reinterpret_cast<uint64_t>(v89);
    TASSIGN(v88, v90);
    pto::Shape<1, 1, 1, 64, 128> v91 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<65536, 65536, 65536, 1024, 1> v92 = pto::Stride<65536, 65536, 65536, 1024, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND> v93 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v13 + (unsigned) v86 * (unsigned) v12), v91, v92);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    TLOAD(v88, v93);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v94 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    TASSIGN(v94, v16);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v95 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v8);
    __ubuf__ float* v96 = v94.data();
    uint64_t v97 = reinterpret_cast<uint64_t>(v96);
    TASSIGN(v95, v97);
    pto::Shape<1, 1, 1, 1, 128> v98 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<1024, 1024, 1024, 1024, 1> v99 = pto::Stride<1024, 1024, 1024, 1024, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND> v100 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<1024, 1024, 1024, 1024, 1>, pto::Layout::ND>(v3 + (v5 + v5 * (unsigned) v13 + (unsigned) v86 * (unsigned) v12), v98, v99);
    TLOAD(v95, v100);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v101 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v101, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v102 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v103 = v101.data();
    uint64_t v104 = reinterpret_cast<uint64_t>(v103);
    TASSIGN(v102, v104);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v102, v88, v82);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v105 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v105, v20);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v106 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ float* v107 = v105.data();
    uint64_t v108 = reinterpret_cast<uint64_t>(v107);
    TASSIGN(v106, v108);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v106, v102, v95);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v109 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    TASSIGN(v109, v15);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v110 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v14, v8);
    __ubuf__ bfloat16_t* v111 = v109.data();
    uint64_t v112 = reinterpret_cast<uint64_t>(v111);
    TASSIGN(v110, v112);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v110, v106, v4);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 64, 128> v113 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<65536, 65536, 65536, 1024, 1> v114 = pto::Stride<65536, 65536, 65536, 1024, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND> v115 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<65536, 65536, 65536, 1024, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v13 + (unsigned) v86 * (unsigned) v12), v113, v114);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v115, v110);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID2);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
