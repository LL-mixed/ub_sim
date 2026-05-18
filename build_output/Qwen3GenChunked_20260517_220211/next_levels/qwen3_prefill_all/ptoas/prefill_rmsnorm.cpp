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

__global__ AICORE void prefill_rmsnorm(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, __gm__ float* v3, int32_t v4, int32_t v5, int32_t v6, int32_t v7, int32_t v8) {
  RoundMode v9 = RoundMode::CAST_ROUND;
  unsigned v10 = 0;
  const int32_t v11 = 2621440;
  const float v12 = 9.99999997E-7f;
  const float v13 = 1.95312503E-4f;
  const int32_t v14 = 128;
  const int32_t v15 = 0;
  const float v16 = 0.0f;
  const int32_t v17 = 40;
  const int32_t v18 = 64;
  const int32_t v19 = 1;
  const int32_t v20 = 5120;
  const int64_t v21 = 256;
  const int64_t v22 = 0;
  const int64_t v23 = 82944;
  const int64_t v24 = 50176;
  const int64_t v25 = 17408;
  const int64_t v26 = 1024;
  const int64_t v27 = 768;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v28 = (size_t) v19;
  size_t v29 = (size_t) v17;
  size_t v30 = (size_t) v15;
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v31, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v33 = v31.data();
  uint64_t v34 = reinterpret_cast<uint64_t>(v33);
  TASSIGN(v32, v34);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  TEXPANDS(v32, v16);
  for (size_t v35 = v30; v35 < v29; v35 += v28) {
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v36, v26);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ bfloat16_t* v38 = v36.data();
    uint64_t v39 = reinterpret_cast<uint64_t>(v38);
    TASSIGN(v37, v39);
    unsigned v40 = (unsigned) v6;
    pto::Shape<1, 1, 1, -1, 128> v41 = pto::Shape<1, 1, 1, -1, 128>(v6);
    pto::Stride<2621440, 2621440, 2621440, 5120, 1> v42 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v43 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v1 + ((v10 + (unsigned) v4 * (unsigned) v11) + (unsigned) v5 * (unsigned) v20 + (unsigned) ((int32_t) (uint32_t) ((int32_t) v35) * (uint32_t) v14) * (unsigned) v19), v41, v42);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v37, v43);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v44 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v44, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v45 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v46 = v44.data();
    uint64_t v47 = reinterpret_cast<uint64_t>(v46);
    TASSIGN(v45, v47);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    TCVT(v45, v37, v9);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v48, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v50 = v48.data();
    uint64_t v51 = reinterpret_cast<uint64_t>(v50);
    TASSIGN(v49, v51);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v52, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v54 = v52.data();
    uint64_t v55 = reinterpret_cast<uint64_t>(v54);
    TASSIGN(v53, v55);
    pipe_barrier(PIPE_V);
    TMUL(v53, v49, v49);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v56, v24);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v57 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v58 = v56.data();
    uint64_t v59 = reinterpret_cast<uint64_t>(v58);
    TASSIGN(v57, v59);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v60 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v19);
    TASSIGN(v60, v23);
    Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v61 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v19);
    __ubuf__ float* v62 = v60.data();
    uint64_t v63 = reinterpret_cast<uint64_t>(v62);
    TASSIGN(v61, v63);
    pipe_barrier(PIPE_V);
    TROWSUM(v61, v53, v57);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v64 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
    TASSIGN(v64, v23);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v65 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
    __ubuf__ float* v66 = v64.data();
    uint64_t v67 = reinterpret_cast<uint64_t>(v66);
    TASSIGN(v65, v67);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v68 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
    TASSIGN(v68, v27);
    Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v69 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
    __ubuf__ float* v70 = v68.data();
    uint64_t v71 = reinterpret_cast<uint64_t>(v70);
    TASSIGN(v69, v71);
    pipe_barrier(PIPE_V);
    TADD(v69, v32, v65);
  }
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v72, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v74 = v72.data();
  uint64_t v75 = reinterpret_cast<uint64_t>(v74);
  TASSIGN(v73, v75);
  pipe_barrier(PIPE_V);
  TMULS(v73, v32, v13);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v76, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v77 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v78 = v76.data();
  uint64_t v79 = reinterpret_cast<uint64_t>(v78);
  TASSIGN(v77, v79);
  pipe_barrier(PIPE_V);
  TADDS(v77, v73, v12);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v80, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v81 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v82 = v80.data();
  uint64_t v83 = reinterpret_cast<uint64_t>(v82);
  TASSIGN(v81, v83);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v84 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v84, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v85 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v86 = v84.data();
  uint64_t v87 = reinterpret_cast<uint64_t>(v86);
  TASSIGN(v85, v87);
  pipe_barrier(PIPE_V);
  TSQRT(v85, v81);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v88 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v88, v27);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v89 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v90 = v88.data();
  uint64_t v91 = reinterpret_cast<uint64_t>(v90);
  TASSIGN(v89, v91);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v92 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  TASSIGN(v92, v22);
  Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v93 = Tile<TileType::Vec, float, 1, 64, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v18);
  __ubuf__ float* v94 = v92.data();
  uint64_t v95 = reinterpret_cast<uint64_t>(v94);
  TASSIGN(v93, v95);
  pipe_barrier(PIPE_V);
  TRECIP(v93, v89);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v96 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v19);
  TASSIGN(v96, v22);
  Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v97 = Tile<TileType::Vec, float, 64, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v19);
  __ubuf__ float* v98 = v96.data();
  uint64_t v99 = reinterpret_cast<uint64_t>(v98);
  TASSIGN(v97, v99);
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID1);
  for (size_t v100 = v30; v100 < v29; v100 += v28) {
    int32_t v101 = (int32_t) ((uint32_t) ((int32_t) v100) * (uint32_t) v14);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v102 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v102, v26);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v103 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ bfloat16_t* v104 = v102.data();
    uint64_t v105 = reinterpret_cast<uint64_t>(v104);
    TASSIGN(v103, v105);
    unsigned v106 = (unsigned) v6;
    pto::Shape<1, 1, 1, -1, 128> v107 = pto::Shape<1, 1, 1, -1, 128>(v6);
    pto::Stride<2621440, 2621440, 2621440, 5120, 1> v108 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v109 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v1 + ((v10 + (unsigned) v4 * (unsigned) v11) + (unsigned) v5 * (unsigned) v20 + (unsigned) v101 * (unsigned) v19), v107, v108);
    wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
    TLOAD(v103, v109);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v110 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v110, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v111 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v112 = v110.data();
    uint64_t v113 = reinterpret_cast<uint64_t>(v112);
    TASSIGN(v111, v113);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
    TCVT(v111, v103, v9);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v114 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v114, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v115 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v116 = v114.data();
    uint64_t v117 = reinterpret_cast<uint64_t>(v116);
    TASSIGN(v115, v117);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v118 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v14);
    TASSIGN(v118, v21);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v119 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v19, v14);
    __ubuf__ float* v120 = v118.data();
    uint64_t v121 = reinterpret_cast<uint64_t>(v120);
    TASSIGN(v119, v121);
    pto::Shape<1, 1, 1, 1, 128> v122 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<5120, 5120, 5120, 5120, 1> v123 = pto::Stride<5120, 5120, 5120, 5120, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND> v124 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<5120, 5120, 5120, 5120, 1>, pto::Layout::ND>(v3 + (v10 + (unsigned) v7 * (unsigned) v20 + (unsigned) v101 * (unsigned) v19), v122, v123);
    TLOAD(v119, v124);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v125 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v125, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v126 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v127 = v125.data();
    uint64_t v128 = reinterpret_cast<uint64_t>(v127);
    TASSIGN(v126, v128);
    pipe_barrier(PIPE_V);
    TROWEXPANDMUL(v126, v115, v97);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v129 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v129, v25);
    Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v130 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ float* v131 = v129.data();
    uint64_t v132 = reinterpret_cast<uint64_t>(v131);
    TASSIGN(v130, v132);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID2);
    TCOLEXPANDMUL(v130, v126, v119);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v133 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    TASSIGN(v133, v26);
    Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v134 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v18, v14);
    __ubuf__ bfloat16_t* v135 = v133.data();
    uint64_t v136 = reinterpret_cast<uint64_t>(v135);
    TASSIGN(v134, v136);
    pipe_barrier(PIPE_V);
    TCVT(v134, v130, v9);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 64, 128> v137 = pto::Shape<1, 1, 1, 64, 128>();
    pto::Stride<327680, 327680, 327680, 5120, 1> v138 = pto::Stride<327680, 327680, 327680, 5120, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v139 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v2 + (v10 + v10 * (unsigned) v20 + (unsigned) v101 * (unsigned) v19), v137, v138);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v139, v134);
    set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
