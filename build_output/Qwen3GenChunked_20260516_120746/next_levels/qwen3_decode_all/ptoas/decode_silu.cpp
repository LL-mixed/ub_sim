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

__global__ AICORE void decode_silu(__gm__ float* v1, __gm__ float* v2, __gm__ bfloat16_t* v3) {
  RoundMode v4 = RoundMode::CAST_ROUND;
  unsigned v5 = 0;
  const float v6 = 1.0f;
  const int32_t v7 = 3072;
  const int32_t v8 = 1;
  const int32_t v9 = 256;
  const int32_t v10 = 16;
  const int64_t v11 = 0;
  const int64_t v12 = 57344;
  const int64_t v13 = 40960;
  const int64_t v14 = 24576;
  const int64_t v15 = 8192;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v16, v15);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v17 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v18 = v16.data();
  uint64_t v19 = reinterpret_cast<uint64_t>(v18);
  TASSIGN(v17, v19);
  pto::Shape<1, 1, 1, 16, 256> v20 = pto::Shape<1, 1, 1, 16, 256>();
  pto::Stride<4096, 4096, 4096, 256, 1> v21 = pto::Stride<4096, 4096, 4096, 256, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v22 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v9 + v5 * (unsigned) v8), v20, v21);
  TLOAD(v17, v22);
  set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v23, v14);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v25 = v23.data();
  uint64_t v26 = reinterpret_cast<uint64_t>(v25);
  TASSIGN(v24, v26);
  pto::Shape<1, 1, 1, 16, 256> v27 = pto::Shape<1, 1, 1, 16, 256>();
  pto::Stride<4096, 4096, 4096, 256, 1> v28 = pto::Stride<4096, 4096, 4096, 256, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND> v29 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<4096, 4096, 4096, 256, 1>, pto::Layout::ND>(v2 + (v5 + v5 * (unsigned) v9 + v5 * (unsigned) v8), v27, v28);
  TLOAD(v24, v29);
  set_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v30, v13);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v31 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v32 = v30.data();
  uint64_t v33 = reinterpret_cast<uint64_t>(v32);
  TASSIGN(v31, v33);
  wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  TNEG(v31, v17);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v34 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v34, v13);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v35 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v36 = v34.data();
  uint64_t v37 = reinterpret_cast<uint64_t>(v36);
  TASSIGN(v35, v37);
  pipe_barrier(PIPE_V);
  TEXP(v35, v31);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v38 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v38, v13);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v39 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v40 = v38.data();
  uint64_t v41 = reinterpret_cast<uint64_t>(v40);
  TASSIGN(v39, v41);
  pipe_barrier(PIPE_V);
  TADDS(v39, v35, v6);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v42 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v42, v12);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v43 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v44 = v42.data();
  uint64_t v45 = reinterpret_cast<uint64_t>(v44);
  TASSIGN(v43, v45);
  pipe_barrier(PIPE_V);
  TRECIP(v43, v39);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v46 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v46, v15);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v47 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v48 = v46.data();
  uint64_t v49 = reinterpret_cast<uint64_t>(v48);
  TASSIGN(v47, v49);
  pipe_barrier(PIPE_V);
  TMUL(v47, v17, v43);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v50 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v50, v15);
  Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v51 = Tile<TileType::Vec, float, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ float* v52 = v50.data();
  uint64_t v53 = reinterpret_cast<uint64_t>(v52);
  TASSIGN(v51, v53);
  pipe_barrier(PIPE_V);
  wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID1);
  TMUL(v51, v47, v24);
  Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v54 = Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  TASSIGN(v54, v11);
  Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v55 = Tile<TileType::Vec, bfloat16_t, 16, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v9);
  __ubuf__ bfloat16_t* v56 = v54.data();
  uint64_t v57 = reinterpret_cast<uint64_t>(v56);
  TASSIGN(v55, v57);
  pipe_barrier(PIPE_V);
  TCVT(v55, v51, v4);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  pto::Shape<1, 1, 1, 16, 256> v58 = pto::Shape<1, 1, 1, 16, 256>();
  pto::Stride<49152, 49152, 49152, 3072, 1> v59 = pto::Stride<49152, 49152, 49152, 3072, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<49152, 49152, 49152, 3072, 1>, pto::Layout::ND> v60 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 256>, pto::Stride<49152, 49152, 49152, 3072, 1>, pto::Layout::ND>(v3 + (v5 + v5 * (unsigned) v7 + v5 * (unsigned) v8), v58, v59);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  TSTORE(v60, v55);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
