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
  const int32_t v11 = 2;
  const int32_t v12 = 16;
  const int32_t v13 = 64;
  const int32_t v14 = 8;
  const int32_t v15 = 0;
  const int32_t v16 = 1;
  const int32_t v17 = 256;
  const int64_t v18 = 8192;
  const int64_t v19 = 0;
  const int64_t v20 = 57408;
  const int64_t v21 = 41024;
  const int64_t v22 = 24640;
  const int64_t v23 = 8256;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  size_t v24 = (size_t) v16;
  size_t v25 = (size_t) v15;
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
  for (size_t v26 = v25; v26 < ((size_t) v14); v26 += v24) {
    for (size_t v27 = v25; v27 < ((size_t) v13); v27 += v24) {
      int32_t v28 = (int32_t) ((uint32_t) v5 + (uint32_t) ((int32_t) v27));
      __gm__ float* v29;
      __gm__ float* v30;
      __gm__ bfloat16_t* v31;
      if (v28 < v6) {
        int32_t v32 = (int32_t) ((uint32_t) v7 - (uint32_t) ((int32_t) (uint32_t) v28 * (uint32_t) v17));
        int32_t v33 = v32 < v17 ? v32 : v17;
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v34 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v33);
        TASSIGN(v34, v23);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v33);
        __ubuf__ float* v36 = v34.data();
        uint64_t v37 = reinterpret_cast<uint64_t>(v36);
        TASSIGN(v35, v37);
        int32_t v38 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) v26) * (uint32_t) v11) + (uint32_t) v28) * (uint32_t) v12);
        unsigned v39 = (unsigned) v33;
        pto::Shape<1, 1, 1, 16, -1> v40 = pto::Shape<1, 1, 1, 16, -1>(v33);
        pto::Stride<4096, 4096, 4096, 256, 1> v41 = pto::Stride<4096, 4096, 4096, 256, 1>();
        GlobalTensor<float, pto::Shape<1, 1, 1, 16, -1>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v42 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, -1>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v4 + (v9 + (unsigned) v38 * (unsigned) v17 + v9 * (unsigned) v16), v40, v41);
        wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
        TLOAD(v35, v42);
        set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v43 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v43, v22);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v44 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ float* v45 = v43.data();
        uint64_t v46 = reinterpret_cast<uint64_t>(v45);
        TASSIGN(v44, v46);
        wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
        pipe_barrier(PIPE_V);
        TFILLPAD(v44, v35);
        set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v47 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v47, v22);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v48 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ float* v49 = v47.data();
        uint64_t v50 = reinterpret_cast<uint64_t>(v49);
        TASSIGN(v48, v50);
        pipe_barrier(PIPE_V);
        TMULS(v48, v44, v10);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v17);
        TASSIGN(v51, v21);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v52 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v17);
        __ubuf__ float* v53 = v51.data();
        uint64_t v54 = reinterpret_cast<uint64_t>(v53);
        TASSIGN(v52, v54);
        Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v16);
        TASSIGN(v55, v20);
        Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v56 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v16);
        __ubuf__ float* v57 = v55.data();
        uint64_t v58 = reinterpret_cast<uint64_t>(v57);
        TASSIGN(v56, v58);
        pipe_barrier(PIPE_V);
        wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
        TROWMAX(v56, v48, v52);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v59 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v59, v22);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v60 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ float* v61 = v59.data();
        uint64_t v62 = reinterpret_cast<uint64_t>(v61);
        TASSIGN(v60, v62);
        pipe_barrier(PIPE_V);
        TROWEXPANDSUB(v60, v48, v56);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v63 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v63, v22);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v64 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ float* v65 = v63.data();
        uint64_t v66 = reinterpret_cast<uint64_t>(v65);
        TASSIGN(v64, v66);
        pipe_barrier(PIPE_V);
        TEXP(v64, v60);
        Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v67 = Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v67, v19);
        Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v68 = Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ bfloat16_t* v69 = v67.data();
        uint64_t v70 = reinterpret_cast<uint64_t>(v69);
        TASSIGN(v68, v70);
        pipe_barrier(PIPE_V);
        TCVT(v68, v64, v8);
        set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v71 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        TASSIGN(v71, v22);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null> v72 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Min, CompactMode::Null>(v12, v17);
        __ubuf__ float* v73 = v71.data();
        uint64_t v74 = reinterpret_cast<uint64_t>(v73);
        TASSIGN(v72, v74);
        pipe_barrier(PIPE_V);
        TCVT(v72, v68, v8);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v75 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v17);
        TASSIGN(v75, v21);
        Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v76 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v17);
        __ubuf__ float* v77 = v75.data();
        uint64_t v78 = reinterpret_cast<uint64_t>(v77);
        TASSIGN(v76, v78);
        Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v79 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v16);
        TASSIGN(v79, v18);
        Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v80 = Tile<TileType::Vec, float, 16, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v12, v16);
        __ubuf__ float* v81 = v79.data();
        uint64_t v82 = reinterpret_cast<uint64_t>(v81);
        TASSIGN(v80, v82);
        pipe_barrier(PIPE_V);
        wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
        TROWSUM(v80, v72, v76);
        set_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
        pto::Shape<1, 1, 1, 16, 256> v83 = pto::Shape<1, 1, 1, 16, 256>();
        pto::Stride<4096, 4096, 4096, 256, 1> v84 = pto::Stride<4096, 4096, 4096, 256, 1>();
        GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v85 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v3 + (v9 + (unsigned) v38 * (unsigned) v17 + v9 * (unsigned) v16), v83, v84);
        wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
        TSTORE(v85, v68);
        pto::Shape<1, 1, 1, 16, 1> v86 = pto::Shape<1, 1, 1, 16, 1>();
        pto::Stride<16, 16, 16, 1, 256> v87 = pto::Stride<16, 16, 16, 1, 256>();
        GlobalTensor<float, pto::Shape<1, 1, 1, 16, 1>, pto::Stride<16, 16, 16, 1, 256>, pto::Layout::DN> v88 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 1>, pto::Stride<16, 16, 16, 1, 256>, pto::Layout::DN>(v2 + (v9 + (unsigned) v38 * (unsigned) v16 + v9 * (unsigned) v17), v86, v87);
        TSTORE(v88, v56);
        set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
        pto::Shape<1, 1, 1, 16, 1> v89 = pto::Shape<1, 1, 1, 16, 1>();
        pto::Stride<16, 16, 16, 1, 256> v90 = pto::Stride<16, 16, 16, 1, 256>();
        GlobalTensor<float, pto::Shape<1, 1, 1, 16, 1>, pto::Stride<16, 16, 16, 1, 256>, pto::Layout::DN> v91 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 1>, pto::Stride<16, 16, 16, 1, 256>, pto::Layout::DN>(v1 + (v9 + (unsigned) v38 * (unsigned) v16 + v9 * (unsigned) v17), v89, v90);
        wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
        TSTORE(v91, v80);
        set_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
        v29 = v1;
        v30 = v2;
        v31 = v3;
      } else {
        v29 = v1;
        v30 = v2;
        v31 = v3;
      };
    };
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID1);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
