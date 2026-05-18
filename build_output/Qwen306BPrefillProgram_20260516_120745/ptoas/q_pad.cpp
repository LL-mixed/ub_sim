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

__global__ AICORE void q_pad(__gm__ bfloat16_t* v1, int32_t v2) {
  unsigned v3 = 0;
  RoundMode v4 = RoundMode::CAST_ROUND;
  const int32_t v5 = 2;
  const int32_t v6 = 16;
  const float v7 = 0.0f;
  const int32_t v8 = 14;
  const int32_t v9 = 8;
  const int32_t v10 = 1;
  const int32_t v11 = 128;
  const int64_t v12 = 7168;
  const int64_t v13 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  for (size_t v14 = (size_t) v2; v14 < ((size_t) ((int32_t) (uint32_t) v2 + (uint32_t) v9)); v14 += (size_t) v10) {
    Tile<TileType::Vec, float, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, float, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    TASSIGN(v15, v13);
    Tile<TileType::Vec, float, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, float, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    __ubuf__ float* v17 = v15.data();
    uint64_t v18 = reinterpret_cast<uint64_t>(v17);
    TASSIGN(v16, v18);
    pipe_barrier(PIPE_V);
    TEXPANDS(v16, v7);
    Tile<TileType::Vec, bfloat16_t, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v19 = Tile<TileType::Vec, bfloat16_t, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    TASSIGN(v19, v12);
    Tile<TileType::Vec, bfloat16_t, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v20 = Tile<TileType::Vec, bfloat16_t, 14, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v11);
    __ubuf__ bfloat16_t* v21 = v19.data();
    uint64_t v22 = reinterpret_cast<uint64_t>(v21);
    TASSIGN(v20, v22);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v20, v16, v4);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 14, 128> v23 = pto::Shape<1, 1, 1, 14, 128>();
    pto::Stride<1792, 1792, 1792, 128, 1> v24 = pto::Stride<1792, 1792, 1792, 128, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 14, 128>, pto::Stride<1792, 1792, 1792, 128, 1>, pto::Layout::ND> v25 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 14, 128>, pto::Stride<1792, 1792, 1792, 128, 1>, pto::Layout::ND>(v1 + (v3 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) v14) * (uint32_t) v6) + (uint32_t) v5) * (unsigned) v11 + v3 * (unsigned) v10), v23, v24);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v25, v20);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
