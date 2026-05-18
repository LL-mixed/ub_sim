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

__global__ AICORE void softmax(__gm__ float* v1, __gm__ float* v2, __gm__ bfloat16_t* v3, __gm__ float* v4, int32_t v5, int32_t v6, int32_t v7) {
  RoundMode v8 = RoundMode::CAST_ROUND;
  unsigned v9 = 0;
  const float v10 = 0.0883883461f;
  const int32_t v11 = 8;
  const int32_t v12 = 5;
  const int32_t v13 = 64;
  const int32_t v14 = 0;
  const int32_t v15 = 256;
  const int32_t v16 = 1;
  const int32_t v17 = 16;
  const int64_t v18 = 4096;
  const int64_t v19 = 0;
  const int64_t v20 = 28704;
  const int64_t v21 = 20512;
  const int64_t v22 = 12320;
  const int64_t v23 = 4128;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
  for (size_t v24 = (size_t) v14; v24 < ((size_t) v13); v24 += (size_t) v16) {
    int32_t v25 = (int32_t) ((uint32_t) v5 + (uint32_t) ((int32_t) v24));
    __gm__ float* v26;
    __gm__ float* v27;
    __gm__ bfloat16_t* v28;
    if (v25 < v6) {
      int32_t v29 = (int32_t) ((uint32_t) v7 - (uint32_t) ((int32_t) (uint32_t) v25 * (uint32_t) v15));
      int32_t v30 = v29 < v15 ? v29 : v15;
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v30);
      TASSIGN(v31, v23);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v30);
      __ubuf__ float* v33 = v31.data();
      uint64_t v34 = reinterpret_cast<uint64_t>(v33);
      TASSIGN(v32, v34);
      int32_t v35 = (int32_t) ((uint32_t) v25 * (uint32_t) v17);
      unsigned v36 = (unsigned) v30;
      pto::Shape<1, 1, 1, 5, -1> v37 = pto::Shape<1, 1, 1, 5, -1>(v30);
      pto::Stride<1280, 1280, 1280, 256, 1> v38 = pto::Stride<1280, 1280, 1280, 256, 1>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 5, -1>, pto::Stride<1280, 1280, 1280, 256, 1>, pto::Layout::ND> v39 = GlobalTensor<float, pto::Shape<1, 1, 1, 5, -1>, pto::Stride<1280, 1280, 1280, 256, 1>, pto::Layout::ND>(v4 + (v9 + (unsigned) v35 * (unsigned) v15 + v9 * (unsigned) v16), v37, v38);
      wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
      TLOAD(v32, v39);
      set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v40 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v40, v22);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v41 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ float* v42 = v40.data();
      uint64_t v43 = reinterpret_cast<uint64_t>(v42);
      TASSIGN(v41, v43);
      wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
      pipe_barrier(PIPE_V);
      TFILLPAD(v41, v32);
      set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v44 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v44, v22);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v45 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ float* v46 = v44.data();
      uint64_t v47 = reinterpret_cast<uint64_t>(v46);
      TASSIGN(v45, v47);
      pipe_barrier(PIPE_V);
      TMULS(v45, v41, v10);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v48 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v15);
      TASSIGN(v48, v21);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v49 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v15);
      __ubuf__ float* v50 = v48.data();
      uint64_t v51 = reinterpret_cast<uint64_t>(v50);
      TASSIGN(v49, v51);
      Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v16);
      TASSIGN(v52, v20);
      Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v53 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v16);
      __ubuf__ float* v54 = v52.data();
      uint64_t v55 = reinterpret_cast<uint64_t>(v54);
      TASSIGN(v53, v55);
      pipe_barrier(PIPE_V);
      wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
      TROWMAX(v53, v45, v49);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v56 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v56, v22);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v57 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ float* v58 = v56.data();
      uint64_t v59 = reinterpret_cast<uint64_t>(v58);
      TASSIGN(v57, v59);
      pipe_barrier(PIPE_V);
      TROWEXPANDSUB(v57, v45, v53);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v60 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v60, v22);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v61 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ float* v62 = v60.data();
      uint64_t v63 = reinterpret_cast<uint64_t>(v62);
      TASSIGN(v61, v63);
      pipe_barrier(PIPE_V);
      TEXP(v61, v57);
      Tile<TileType::Vec, bfloat16_t, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v64 = Tile<TileType::Vec, bfloat16_t, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v64, v19);
      Tile<TileType::Vec, bfloat16_t, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v65 = Tile<TileType::Vec, bfloat16_t, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ bfloat16_t* v66 = v64.data();
      uint64_t v67 = reinterpret_cast<uint64_t>(v66);
      TASSIGN(v65, v67);
      pipe_barrier(PIPE_V);
      TCVT(v65, v61, v8);
      set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v68 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      TASSIGN(v68, v22);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v69 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v11, v15);
      __ubuf__ float* v70 = v68.data();
      uint64_t v71 = reinterpret_cast<uint64_t>(v70);
      TASSIGN(v69, v71);
      pipe_barrier(PIPE_V);
      TCVT(v69, v65, v8);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v72 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v15);
      TASSIGN(v72, v21);
      Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v73 = Tile<TileType::Vec, float, 8, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v15);
      __ubuf__ float* v74 = v72.data();
      uint64_t v75 = reinterpret_cast<uint64_t>(v74);
      TASSIGN(v73, v75);
      Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v16);
      TASSIGN(v76, v18);
      Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v77 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v16);
      __ubuf__ float* v78 = v76.data();
      uint64_t v79 = reinterpret_cast<uint64_t>(v78);
      TASSIGN(v77, v79);
      pipe_barrier(PIPE_V);
      wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
      TROWSUM(v77, v69, v73);
      set_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
      pto::Shape<1, 1, 1, 8, 256> v80 = pto::Shape<1, 1, 1, 8, 256>();
      pto::Stride<2048, 2048, 2048, 256, 1> v81 = pto::Stride<2048, 2048, 2048, 256, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 8, 256>, pto::Stride<2048, 2048, 2048, 256, 1>, pto::Layout::ND> v82 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 8, 256>, pto::Stride<2048, 2048, 2048, 256, 1>, pto::Layout::ND>(v3 + (v9 + (unsigned) v35 * (unsigned) v15 + v9 * (unsigned) v16), v80, v81);
      wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
      TSTORE(v82, v65);
      int32_t v83 = (int32_t) ((uint32_t) v25 * (uint32_t) v11);
      pto::Shape<1, 1, 1, 8, 1> v84 = pto::Shape<1, 1, 1, 8, 1>();
      pto::Stride<8, 8, 8, 1, 16> v85 = pto::Stride<8, 8, 8, 1, 16>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 16>, pto::Layout::DN> v86 = GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 16>, pto::Layout::DN>(v2 + (v9 + (unsigned) v83 * (unsigned) v16 + v9 * (unsigned) v17), v84, v85);
      TSTORE(v86, v53);
      set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
      pto::Shape<1, 1, 1, 8, 1> v87 = pto::Shape<1, 1, 1, 8, 1>();
      pto::Stride<8, 8, 8, 1, 16> v88 = pto::Stride<8, 8, 8, 1, 16>();
      GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 16>, pto::Layout::DN> v89 = GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 16>, pto::Layout::DN>(v1 + (v9 + (unsigned) v83 * (unsigned) v16 + v9 * (unsigned) v17), v87, v88);
      wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
      TSTORE(v89, v77);
      set_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
      v26 = v1;
      v27 = v2;
      v28 = v3;
    } else {
      v26 = v1;
      v27 = v2;
      v28 = v3;
    };
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
