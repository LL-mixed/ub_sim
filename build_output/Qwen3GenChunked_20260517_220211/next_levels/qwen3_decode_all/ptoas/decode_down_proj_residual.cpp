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

__global__ AICORE void decode_down_proj_residual(__gm__ float* v1, __gm__ float* v2, __gm__ bfloat16_t* v3, int32_t v4, int32_t v5) {
  RoundMode v6 = RoundMode::CAST_ROUND;
  unsigned v7 = 0;
  const int32_t v8 = 5120;
  const int32_t v9 = 1;
  const int32_t v10 = 128;
  const int32_t v11 = 16;
  const int64_t v12 = 16384;
  const int64_t v13 = 8192;
  const int64_t v14 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  TASSIGN(v15, v14);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  __ubuf__ float* v17 = v15.data();
  uint64_t v18 = reinterpret_cast<uint64_t>(v17);
  TASSIGN(v16, v18);
  pto::Shape<1, 1, 1, 16, 128> v19 = pto::Shape<1, 1, 1, 16, 128>();
  pto::Stride<2048, 2048, 2048, 128, 1> v20 = pto::Stride<2048, 2048, 2048, 128, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND> v21 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<2048, 2048, 2048, 128, 1>, pto::Layout::ND>(v1 + (v7 + v7 * (unsigned) v10 + v7 * (unsigned) v9), v19, v20);
  TLOAD(v16, v21);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v22 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  TASSIGN(v22, v13);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  __ubuf__ float* v24 = v22.data();
  uint64_t v25 = reinterpret_cast<uint64_t>(v24);
  TASSIGN(v23, v25);
  pto::Shape<1, 1, 1, 16, 128> v26 = pto::Shape<1, 1, 1, 16, 128>();
  pto::Stride<81920, 81920, 81920, 5120, 1> v27 = pto::Stride<81920, 81920, 81920, 5120, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v28 = GlobalTensor<float, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v2 + (v7 + v7 * (unsigned) v8 + (unsigned) v4 * (unsigned) v9), v26, v27);
  TLOAD(v23, v28);
  set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  TASSIGN(v29, v14);
  Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v30 = Tile<TileType::Vec, float, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  __ubuf__ float* v31 = v29.data();
  uint64_t v32 = reinterpret_cast<uint64_t>(v31);
  TASSIGN(v30, v32);
  wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  TADD(v30, v16, v23);
  Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v33 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  TASSIGN(v33, v12);
  Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v34 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  __ubuf__ bfloat16_t* v35 = v33.data();
  uint64_t v36 = reinterpret_cast<uint64_t>(v35);
  TASSIGN(v34, v36);
  pipe_barrier(PIPE_V);
  TCVT(v34, v30, v6);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  pto::Shape<1, 1, 1, 16, 128> v37 = pto::Shape<1, 1, 1, 16, 128>();
  pto::Stride<81920, 81920, 81920, 5120, 1> v38 = pto::Stride<81920, 81920, 81920, 5120, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND> v39 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 16, 128>, pto::Stride<81920, 81920, 81920, 5120, 1>, pto::Layout::ND>(v3 + (v7 + (unsigned) v5 * (unsigned) v8 + (unsigned) v4 * (unsigned) v9), v37, v38);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  TSTORE(v39, v34);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
