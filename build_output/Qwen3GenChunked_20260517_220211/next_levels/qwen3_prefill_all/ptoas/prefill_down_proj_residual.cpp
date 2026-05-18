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

__global__ AICORE void prefill_down_proj_residual(__gm__ float* v1, __gm__ float* v2, __gm__ bfloat16_t* v3, int32_t v4, int32_t v5, int32_t v6, int32_t v7) {
  RoundMode v8 = RoundMode::CAST_ROUND;
  unsigned v9 = 0;
  const int32_t v10 = 128;
  const int32_t v11 = 1;
  const int32_t v12 = 5120;
  const int32_t v13 = 64;
  const int64_t v14 = 65536;
  const int64_t v15 = 32768;
  const int64_t v16 = 0;
  const int32_t v17 = 2621440;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v18 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  TASSIGN(v18, v16);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v19 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  __ubuf__ float* v20 = v18.data();
  uint64_t v21 = reinterpret_cast<uint64_t>(v20);
  TASSIGN(v19, v21);
  pto::Shape<1, 1, 1, 64, 128> v22 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<8192, 8192, 8192, 128, 1> v23 = pto::Stride<8192, 8192, 8192, 128, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<8192, 8192, 8192, 128, 1>, pto::Layout::ND> v24 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<8192, 8192, 8192, 128, 1>, pto::Layout::ND>(v2 + (v9 + v9 * (unsigned) v10 + v9 * (unsigned) v11), v22, v23);
  TLOAD(v19, v24);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  TASSIGN(v25, v15);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v26 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  __ubuf__ float* v27 = v25.data();
  uint64_t v28 = reinterpret_cast<uint64_t>(v27);
  TASSIGN(v26, v28);
  pto::Shape<1, 1, 1, 64, 128> v29 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<327680, 327680, 327680, 5120, 1> v30 = pto::Stride<327680, 327680, 327680, 5120, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND> v31 = GlobalTensor<float, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<327680, 327680, 327680, 5120, 1>, pto::Layout::ND>(v1 + (v9 + v9 * (unsigned) v12 + (unsigned) v4 * (unsigned) v11), v29, v30);
  TLOAD(v26, v31);
  set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v32 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  TASSIGN(v32, v16);
  Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v33 = Tile<TileType::Vec, float, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  __ubuf__ float* v34 = v32.data();
  uint64_t v35 = reinterpret_cast<uint64_t>(v34);
  TASSIGN(v33, v35);
  wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
  TADD(v33, v19, v26);
  Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v36 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  TASSIGN(v36, v14);
  Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v37 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v13, v10);
  __ubuf__ bfloat16_t* v38 = v36.data();
  uint64_t v39 = reinterpret_cast<uint64_t>(v38);
  TASSIGN(v37, v39);
  pipe_barrier(PIPE_V);
  TCVT(v37, v33, v8);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  pto::Shape<1, 1, 1, 64, 128> v40 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<2621440, 2621440, 2621440, 5120, 1> v41 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v42 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v3 + ((v9 + (unsigned) v5 * (unsigned) v17) + (unsigned) v6 * (unsigned) v12 + (unsigned) v4 * (unsigned) v11), v40, v41);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  TSTORE(v42, v37);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
